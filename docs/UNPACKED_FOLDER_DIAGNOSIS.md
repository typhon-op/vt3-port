# "Use an unpacked game folder" — why it fails, and what to do about it

Scope: static analysis of the installer and the native entry point on commit `afdf2e1`
(branch `freebuff/d5cb375d0b21dd443a23cb88`). **No device and no emulator was available for this
investigation**, so nothing below is a reproduction: every claim is either *certain from the code*
(with a `file:line` reference) or marked as *probable* with the log line that would confirm it.

Files that matter:

| File | Role |
| --- | --- |
| `android/game/java/net/vt3port/game/Installer.java` | setup screen, install thread, switch between disc image and folder |
| `android/game/java/net/vt3port/game/StorageAccess.java` | All-Files gate, SAF tree copy, `looksInstalled()` |
| `android/game/jni/install.c` | ISO extraction layout (`<out>/<group>/<dir>/<file>`) |
| `runtime/src/platform/android_entry.c` | reads `<root>/game_root`, sets `VT3_GAME_ROOT` |

---

## 1. What the feature promises, and what the code does

Documented promise (`README.md`, "Game files and storage access"):

> **Use an unpacked game folder** — if you already have the PC install unpacked on the phone, the
> game reads it where it lies instead of unpacking a 4.5 GB image.

Same claim twice in the class, and in the picker branch:

- `Installer.java:33` — "unpacks **or copies** the game data"
- `Installer.java:240` — `sourceDir = dir; /* a real path: the runtime reads it in place, nothing copied */`
- `Installer.java:409-410` — "The folder path: **in place** when it is already a real path, through a
  copy when it came from the picker."

What actually happens (`Installer.java:352-355`):

```java
File data2 = new File(base, "data2");
if (sourceDir != null || sourceTree != null) {
    copyGame(data2);          // ALWAYS copies, for both source kinds
} else {
    unpackDisc(data2);
}
```

`copyGame()` (`:410-422`) then calls `copyDir(sourceDir, data2, …)` for a real path and
`StorageAccess.copyTree(…)` for a SAF tree. **There is no branch that uses the folder where it
lies.** `git log -S"in place"` shows the comment has been there since the feature was added in
`e60126a`; the in-place path was never implemented. So "reads it where it lies" is false for both
routes, and the phone's own setup text admits it: `source_tree_ready` = "Folder chosen. It is copied
into app storage while installing."

Consequence: the feature currently behaves exactly like "unpack the ISO", except that the source is
a folder instead of an ISO — and it costs a full second copy of the data.

---

## 2. Certain defects, ranked by how badly they break the feature

### 2.1 The picked folder must be the one that *directly* contains `Resource` — and only one of the two routes checks that

`StorageAccess.looksInstalled()` (`:273-275`) is `dir/Resource` must be a directory. It is applied:

- on the All-Files route at pick time (`Installer.java:236`) and again before the copy (`:340`);
- **not at all** on the SAF route (`onActivityResult`, `Installer.java:161-173`), and
  `StorageAccess.copyTree()` copies the *children* of the picked tree into `<base>/data2`.

So the two routes disagree about what a valid pick is:

| Pick | All-Files route | SAF route |
| --- | --- | --- |
| `…/data2` (has `Resource`) | accepted | accepted |
| `…/` install root (has `data1/`, `data2/`, `VT3.exe`) | refused immediately: "…does not look like an unpacked Virtua Tennis 3 folder: it has no Resource folder" | **accepted**, copies both trees + cabs, then fails at the very end: "Nothing was installed. The game data is incomplete." |

A PC install root is the natural thing to pick, and it is roughly the *worst* pick: both
byte-identical `Resource` trees plus the `.cab` files, i.e. up to ~9 GB copied, and a failure only
after the copy finishes. Because `copyTree`/`copyDir` copy **everything** and never flatten a single
`data2` child, the resulting tree is `<base>/data2/data2/Resource` and the final check
`new File(data2, "Resource").isDirectory()` (`:359`) throws.

Symptom to expect: a long "Copying game files…" then "Setup failed: Nothing was installed. The game
data is incomplete." — with several GB left behind in app storage.

### 2.2 The free-space gate measures the destination, not the job

`NEED_BYTES = 5L << 30` (`:56`) is checked against the destination only (`:338`), and the message
says "about 4.5 GB" (`dest_no_space`). A single extracted tree is ~4.5 GB, so with the source
already on the phone the copy needs ~4.5 GB *of new* space, while the filesystem also holds the
source. With the SAF mis-pick above the job is ~9 GB and the gate still passes at 5 GiB.

### 2.3 A failed or interrupted install is never cleaned up, and a large copy cannot be cancelled

- No `delete` anywhere after a failed copy; a partial `<base>/data2` stays and is silently reused
  (each file is opened with `FileOutputStream`, so files are truncated and rewritten, but stale
  files from an earlier, different source remain and can satisfy the final `Resource` check while
  the tree is a mix).
- `onBackPressed()` is disabled while `busy` (`:126-128`) and no other path sets `busy = false`
  mid-install, so a stuck multi-GB copy can only be stopped by killing the app.
- `docs/HANDOFF.md` already records "A large folder copy also has no resume".

### 2.4 A remembered folder silently overrides a newly picked disc image

`pick(PICK_ISO)` (`:139-146`) sets `iso` and never clears `sourceDir`/`sourceTree`. Since
`install()` prefers the folder (`:352`), a player who tried a folder once and then picked the ISO
gets the folder install again — the ISO is ignored with no message.

### 2.5 Case-sensitive `Resource` test

`looksInstalled()` compares `"Resource"` exactly, while the runtime resolves path components
case-insensitively (`runtime/shim/src/kernel32_file.c:31-73`). A tree whose folder is `resource` is
refused by the installer but would work at runtime.

### 2.6 The picker route can be permanently refused on providers that do not persist grants

`StorageAccess.remember()` (`:106-118`) swallows a failed `takePersistableUriPermission`, and
`onActivityResult` then requires `granted(...)` (`:119-125`) **and** `listable(...)`. On a provider
that does not allow a persisted grant, `granted()` is always false, so the folder is forgotten and
the player sees `source_tree_failed`: "That folder could not be opened. Its permission may have been
revoked: pick it again." — forever, for that folder. The grant only needs to survive one install
attempt; a live `listable()` query is the honest test.

### 2.7 After a successful install there is no way back into setup

`onCreate` (`:76-82`) launches the game as soon as `<root>/install.ok` exists. There is no
"change install folder / reinstall" action anywhere, so a wrong or half-broken folder install can
only be corrected by clearing app data (which also deletes `image.bin`, the shader pack and the
logs). A folder install that "doesn't work" therefore tends to stay that way.

### 2.8 Two requirements that are easy to miss on the screen

- `canInstall()` (`:200-203`) requires a separate **VT3.exe** pick even when the folder already
  contains `VT3.exe`; `buildImage()` (`:460-487`) then rejects anything whose flattened image
  SHA-256 is not `52be3cda…fc80bf` (the English v1.0.1 exe). A folder from another language or
  another build of the game cannot install, and the message ("That VT3.exe is not the v1.0.1 build
  this port needs") does not say the folder route needs the same exe.
- The folder route on the All-Files side needs `MANAGE_EXTERNAL_STORAGE`; without it the button
  silently becomes the SAF picker, which copies.

### 2.9 Minor, but visible

- The `sourceDir` copy has **no progress at all** (`copyDir` accumulates `bytes[0]` and never reports
  it); the bar is an indeterminate spinner for the whole copy, so it looks hung.
- `copyDir` follows symlinks (`f.isDirectory()`), so a cyclic link recurses until `MAX_DEPTH = 32`
  throws "That folder is nested too deeply."
- The generic `install_bad` string is used for both "no source" (`:343`) and "no `Resource` after
  the copy" (`:360`), so the message never identifies which check failed.
- `R.string.source_empty` is unused; `copyTree` throws its own hard-coded "That folder is empty."

---

## 3. How to confirm which one it is on the phone (5 minutes)

The app already logs the whole install (`DiagLog`, "install" and "storage" tags), and the log
survives in `<root>` (`/sdcard/Android/data/net.vt3port.game/files`). On the device:

```
adb logcat -c
# reproduce: open VT3 -> "Use an unpacked game folder" -> pick -> Install
adb logcat -d | grep -iE "install|storage|game_root|VT3_GAME_ROOT"
```

Interpretation:

| Log line | Meaning |
| --- | --- |
| `install started; …; source <path>; all files access granted` | the one line that shows *which* source the install actually used — compare it with what you tapped (2.4 if they differ) |
| `source <content://…>` | you were on the SAF route (no All-Files grant) |
| `install failed` + stack | the exception message names the failing step (`Cannot create …`, `No space left on device`, `install_bad`) |
| `game data complete at <path>` | copy finished and `Resource` was seen; then the fault is at runtime, not install |
| `game data at <path> (chosen in setup)` at launch | the marker was read — confirms `game_root` |
| `<path> is not readable, using app storage` | stale marker, fell back to `<root>/data2` (which may not exist) |

Also worth checking on the device before any of this: `df -h /sdcard` free space, and
`ls /sdcard/Android/data/net.vt3port.game/files/data2/Resource` (does it exist and is it complete?).

---

## 4. Suggested fixes, in the order I would do them

1. **Make the folder route behave as documented — read it in place.** When `sourceDir != null`
   (`looksInstalled()` true), skip `copyGame`, keep `data2 = sourceDir`, and write
   `game_root = sourceDir`. The runtime already supports exactly this: `android_entry.c:138-159`
   sets `VT3_GAME_ROOT` from the marker, and `resolve_path()` opens plain POSIX paths. This removes
   the second copy, the disk-space cliff, and the "why is it copying 4.5 GB again" complaint in one
   change. Gate it on a *write* test (the shim merges `vt3.ini` into the game root; see
   `runtime/shim/src/d3d9_gpu.c:218`), and fall back to the copy when the folder is read-only.
2. **Accept either folder level, and flatten it.** In one helper (used by both routes): if
   `dir/Resource` exists use `dir`; else if exactly one child is a directory containing `Resource`
   (e.g. `data2`), use that child. Apply it at pick time *and* before the copy, on both routes, so
   the SAF mis-pick can no longer destroy 9 GB before failing.
3. **Fix the space gate.** Measure the source (`du`-style walk, capped) and require
   `sourceBytes + 4.5 GB` of free space at the destination when a copy is really needed; update the
   `dest_no_space` text so it is not a fixed 4.5 GB.
4. **Clean up on failure, and let the player cancel.** Delete the partial `<base>/data2` when the
   install throws, show a real byte counter for `copyDir`, and allow Back/cancel to abort the thread.
5. **Make `install.ok` reversible.** Add a "Change install / reinstall" action on the setup screen
   (or a long-press on the logo) that deletes `install.ok` and `game_root` and returns to setup —
   today a bad folder install needs a full app-data clear.
6. **Clear the folder when the disc image is picked** (2.4), and offer the folder's own `VT3.exe`
   automatically (2.8) instead of requiring a second pick.
7. **Relax the grant requirement to a live query** (2.6): require `listable()` before the copy and
   treat a missing persisted grant as only a hint, since the install is a single activity.
8. **Case-insensitive `looksInstalled()`**, and specific failure messages (`install_bad` today
   covers two different causes: no source at all, and no `Resource` after the copy).

None of these is a large change: (1) and (2) are the two that decide whether the feature works at
all; the rest are robustness and honesty about what happened.

---

## 5. What was and was not verified

- **Verified by reading code at `afdf2e1`:** every statement in §1 and §2 that cites a line; the
  native marker path and the case-insensitive resolver in §2.5; the extraction layout in
  `install.c:155-190` (`<out>/<group>/<dir>/<file>`, which is why an extracted tree has `Resource`
  at its top level).
- **Not verified:** any of this on hardware. There was no device, no emulator and no `adb` target in
  this workspace, and `apksigner`/`aapt2` are not installed either. §3 is the instrumentation that
  turns the ranking above into a definite answer; the diagnostic log already contains every string
  it needs.
- The APK in `artifacts/download/` is unchanged by this investigation (nothing was rebuilt or
  edited to produce this note).

---

## 6. Status: what was implemented after this note

This section was added after the fixes below were written; §1–§5 stay as the analysis at `afdf2e1`.

- **(1) Direct mode is real.** `Installer.java` asks `InstallPlan` what to do: a selected folder the
  app may write is used in place (`Mode.DIRECT`), nothing is copied, and `<root>/game_root` gets the
  selected path. `copyGame()` no longer exists. The native support this note assumed is confirmed:
  `tools/recomp/rt/vt3rt.c:398` reads `VT3_GAME_ROOT` and calls `vt3shim_set_game_root()` before the
  shim starts, so `android_entry.c:138-159` is enough for both reads and writes at that path.
- **(2) One folder rule for both pickers** (`GameFolder`): the picked folder or a single child
  holding `Resource`, matched case-insensitively, with `VT3.exe` detected automatically. A SAF
  mis-pick now copies only the valid child, and more than one candidate is refused by name.
- **(3) Grants.** A failed `takePersistableUriPermission()` is logged, not fatal; `listable()` decides.
- **(4) Install state.** Picking the image clears the folder and vice versa, a failed or cancelled
  copy is deleted, Back/Cancel stops a copy, progress is byte-based, the free-space gate is the
  source size plus headroom, and Settings → *Change installation / reinstall* returns to setup
  without clearing app data.
- **(5) Native side untouched.** No C change was needed; the pinned `libvt3android.so` is unchanged
  (hash verified in the APK) and `libvt3install.so` is rebuilt from `android/game/jni`.
- **Review pass.** The tree picker now resolves a folder once (real path first, the picker's listing
  otherwise) instead of twice, and reports the reason from whichever route it used; direct mode no
  longer measures a folder it does not copy; the space gate for an implausibly large source refuses
  with its real size instead of trying to fit it in the unpack gate. Unused members were removed
  (`viaChild()`, `InstallPlan.copyFrom`, `StorageAccess.granted()`/`looksInstalled()`, the unused
  progress-name parameter, `direct_ready`).
- **Still unverified:** a real device launching the game from an external folder. The host test
  (`tools/installtest/InstallHostTest.java`) covers the decisions, not the launch.
