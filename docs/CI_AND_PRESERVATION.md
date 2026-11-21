# Preserving the Android port, rebuilding it, and publishing the APK

This is the handover sheet for a session that picks the project up cold: what is in the repository,
what is generated, where the reusable outputs live, how the signing identity is kept, and how the
debug APK is built and published from GitHub Actions.

The repository is the source of truth for everything the port owns. Nothing game-derived is
committed: not `VT3.exe`, not the translated `vt3.c`/`image.bin`, not the shader pack, not a disc
image, not an extracted data folder. Those are produced from the player's own copy at build or
install time. `.gitignore` enforces it (`*.exe`, `*.iso`, `*.bin`, `*.pak`, `build/`, `*.keystore`).

## 1. What the repository preserves

| Area | Where |
|---|---|
| Android app: installer UI, SAF/All-files access, setup flow, launch, diagnostics screens | `android/game/java/net/vt3port/game/`, `android/game/res/`, `android/game/AndroidManifest.xml` |
| Native Android glue: NativeActivity, JNI bridge, installer (`unshield`), audio, input, lifecycle | `android/game/jni/`, `android/third_party/unshield/` |
| Recompiled-runtime host: kernel32/user32/dsound/d3d9 shims, guest execution, VM allocator | `runtime/src/`, `runtime/include/` |
| Vulkan renderer (shared by Mac and Android), D3D9→Vulkan translation, shader upload | `runtime/src/render/` |
| Audio engine and the AAudio/lifecycle fixes | `runtime/src/audio/`, `runtime/src/platform/android/` |
| Touch controls and controller input | `runtime/src/input/`, `docs/TOUCH_CONTROLS.md` |
| Graphics profiles, thermal/battery behaviour | `runtime/src/render/`, `docs/ANDROID_SETTINGS_REVIEW.md` |
| Diagnostics: async ring log, crash/last-error file, metrics, TXT report, share | `runtime/src/core/log.c`, `runtime/src/core/diag.c`, `android/game/java/net/vt3port/game/Diag*.java`, `Diagnostics.java` |
| Shader sources and translators (SM2 scan, assembler, SPIR-V pack builder) | `tools/formats/shaders/`, `docs/d3d9-shaders.md` |
| Translator / recompiler (VT3.exe → C) and its seeds and table sizes | `tools/recomp/` |
| Build scripts | `build.py`, `android/build_apk.py`, `android/deploy_data.sh` |
| Tests | `runtime/tests/`, `tools/diagtest/`, `tests/selftest.c` |
| CI | `.github/workflows/android-debug.yml` |
| Documentation | `README.md`, `docs/` |

Build configuration is intentionally file-based (no Gradle): `build.py` drives CMake + Ninja for the
native library and `android/build_apk.py` drives `aapt2`/`d8`/`zipalign`/`apksigner`. There is no
dependency download step and no version drift to chase; the pinned toolchain is listed in
`README.md` and repeated in the workflow environment:

- NDK `30.0.16248370`, `platforms;android-35`, `build-tools;36.1.0`, `ANDROID_API 29` (min), 35 (target)
- CMake ≥ 3.22, Ninja, `glslangValidator` (Vulkan SDK or `glslang`), JDK 21 (`javac --release 11`)
- Python 3.10+ with `pefile` and `capstone` for the translator only

## 2. What is generated, and how to reuse it

| Output | Produced by | Reuse |
|---|---|---|
| `build/game/vt3.c`, `build/game/image.bin` | `python build.py inputs --exe VT3.exe` | the whole ARM64 library; without it nothing compiles |
| `build/shaders.pack` | same command (SM2 blobs from the exe + rebuilt D3D shaders, `glslangValidator`) | bundled as `assets/shaders.pack` |
| `build/android/libvt3android.so` | `build.py android` (CMake + NDK clang) | the game library inside the APK; also the `--so` input that repackages without recompiling |
| `build/android/VT3-Android.apk` | `build.py android` → `android/build_apk.py` | the deliverable |

Rebuilding **without translating again** is what keeps small changes cheap:

```sh
python build.py android --game build/game --shaders build/shaders.pack
```

That path skips the translator and the shader pack builder (a few minutes) and goes straight to the
C compile and the APK. UI, Java, resource, manifest and runtime C/C++ changes all rebuild this way.
Only a change to the *translator* (`tools/recomp/`) or to the shader *translation* requires
`build.py inputs --exe` again.

`build.py` still refuses to build from a translation of the wrong game: `image.bin` must hash to
`52be3cda5edb9aa536394fed656e22270d688d74e86db8602fd39a7512fc80bf`.

### Where those inputs are kept

They are game-derived, so they live in **private** stores only, never in git — `docs/BUILD_INPUTS.md`
is the full account. In short, the CI takes the first of these that exists:

1. **GitHub Actions cache**, key `vt3-translated-<IMAGE_SHA256>`: `build/game` + `build/shaders.pack`.
   Fast, but it is a cache: GitHub evicts it after 90 idle days.
2. **Release asset** `vt3-translated-inputs.tar.gz` on the `build-inputs` tag, a release in this
   private repository: the durable copy of the same two paths, refilled by a run with
   `publish_inputs` ticked.
3. **`prebuilt/`** in the repository: `libvt3android.so` + `shaders.pack`, hashes in
   `prebuilt/SHA256SUMS.txt`. No translated C source, so it rebuilds everything Android around the
   preserved library instead of recompiling the native runtime.

With none of them the run stops at "Select the build inputs" and says what to restore; it never
builds from a partial or wrong input. The private cloud workspace of the session that produced the
current APK (`artifacts/`, not committed) still holds `build-inputs/libvt3android.so`,
`build-inputs/shaders.pack`, the signed APK and its `BUILD_INFO.txt`; that is where the two files for
store 3 come from. The translated C source is not there: it was deleted with the build tree as
instructed, and it can only be regenerated from `VT3.exe`.

## 3. The signing identity

- Keystore: `debug.keystore`, alias `vt3port`, store and key password `android` (the standard debug
  passwords), certificate SHA-256
  `6150e1f4d5a195bccf53cc61c1a5384c4f6bc458c5f85dd3c40db18be44607d0` (`CN=vt3 port`).
- `android/build_apk.py` reuses `build/android/debug.keystore` when it exists and only generates a
  key when it does not. Never delete the file; an APK signed with a different key cannot install
  over one that is already installed.
- It is **not** in git (`.gitignore` has `*.keystore`) and must never be committed or uploaded as an
  artifact. Copies kept privately: `artifacts/debug.keystore` in this chat's workspace, and the
  repository secret `ANDROID_DEBUG_KEYSTORE_B64` (base64 of the same file) that the workflow
  restores into `build/android/debug.keystore` before building.
- The workflow verifies the APK's signer digest against the certificate above and fails the job on a
  mismatch, so a wrong or regenerated key can never reach the artifact silently.

## 4. Publishing the APK from GitHub Actions

Workflow: `.github/workflows/android-debug.yml`, entirely on GitHub-hosted runners, three triggers:

- **push to `vt3android`** builds that commit automatically (`paths-ignore` skips prose-only edits),
- **workflow_dispatch** builds on demand, with `retention_days` and an optional `publish_inputs`,
- **schedule**, Mondays 03:17 UTC, which keeps the pipeline and its pinned toolchain exercised.

The `build` job installs JDK 21, the Android command line tools, `platforms;android-35`,
`build-tools;36.1.0` and `ndk;30.0.16248370`; restores the inputs in the order listed in section 2
and validates them (translated `image.bin` against the supported hash, `prebuilt/` against its own
`SHA256SUMS.txt`, and the library inside the finished APK against that hash again); sets the
`versionCode`; runs the native and Java diagnostics tests; builds, packages and signs; verifies
alignment, signature and certificate, package name, `arm64-v8a`, the exported native symbols and the
diagnostics classes; writes `SHA256SUMS.txt` and `BUILD_INFO.txt`; and uploads the artifact. A run
that has the translated source can also publish it as the private release asset, which is how the
durable copy is created without a game file ever reaching a runner.

Artifact `VT3-Android-Debug` holds exactly `VT3-Android.apk`, `SHA256SUMS.txt`, `BUILD_INFO.txt`,
retention `retention_days` (default **7**). A step fails the job if anything else — an executable,
image, source or library — would be uploaded. The `guard` job refuses a public repository, because
the APK contains code and shader data translated from the game and artifacts plus logs of a public
repository are readable by anyone; for a manual run it fails with the setting to change, and for a
push or a scheduled run it reports a warning instead of turning every push red.

Download on the phone: **Actions → VT3 Android Debug → the run → Artifacts → VT3-Android-Debug**
(a GitHub login is required; artifacts of a private repository are visible only to people who can
read it). The zip holds the APK and its two text files.

### Version codes and updates

The workflow sets it for every build: `versionCode = 1000 + github.run_number`, so each published
build is higher than the last without a bot commit, and CI builds sit above the hand-built 0.2.0
(versionCode 2). Android installs an update only when the package name and the signing certificate
match and the `versionCode` is at least the installed one; the workflow checks all three. The
manifest itself still says **2** (`versionName 0.2.0`) for local builds; `BUILD_INFO.txt` records the
code and the `+ci.<run>` version name each artifact was built with.

## 5. What is not verified here, and what blocks a CI-published APK right now

- No device or emulator is available in the cloud workspace, so nothing here proves the game runs:
  launch, Vulkan, audio, touch controls, the diagnostics screen and saving/sharing the report are
  still owed on hardware.
- **The repository is public today** (`isPrivate: false`), and that is the one thing blocking the
  whole pipeline: the APK and any committed translated output contain code and shader data
  generated from Virtua Tennis 3, so publishing them from a public repository is exactly what the
  project rules forbid. The `guard` job therefore refuses to build. Make it private
  (Settings → General → Danger Zone → Change repository visibility → Private) and the same
  workflow, unchanged, starts producing APKs.
- **The signing secret does not exist yet.** Add `ANDROID_DEBUG_KEYSTORE_B64` (base64 of
  `debug.keystore`) under Settings → Secrets and variables → Actions. The job stops with that
  instruction rather than signing with a new key, which could not install as an update.
- **`prebuilt/` is empty.** `prebuilt/README.md` explains what goes in it and how to produce the two
  files from an existing APK; doing it in a private repository makes every push build an APK with
  no other input. With the translated C source published instead, the workflow compiles the native
  side too — that is the mode to aim for, since it is the only one where `runtime/` changes ship.
- This workspace has no GitHub credentials (`gh` is not logged in, no token), so it cannot dispatch
  a run, read a run's logs or download an artifact. The workflow is written, YAML-parsed, every
  shell block parsed with `bash -n`, and its build/verify/report steps were executed here against a
  real APK built by `build.py android --so …` — but no Actions run is claimed, and none is.
