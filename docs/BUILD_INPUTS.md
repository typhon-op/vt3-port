# Build inputs: where the translated game lives, and what CI does when it is not there

The Android APK cannot be built from the repository alone. `runtime/` compiles against `build/game`
— `vt3.c` (the x86 → C translation) and `image.bin` (the guest image) — plus `build/shaders.pack`,
all three generated from the player's own `VT3.exe`. That executable is never committed, uploaded,
printed or published, and the build products derived from it are just as restricted.

`.github/workflows/android-debug.yml` therefore does not ask for a game file at all. It looks for the
inputs in three places, in order, and reports in `BUILD_INFO.txt` which one it used.

## 1. The three stores

| # | Store | What it holds | Held where | How it gets there |
| --- | --- | --- | --- | --- |
| 1 | **Actions cache**, key `vt3-translated-52be3cda…fc80bf` | `build/game`, `build/shaders.pack` | GitHub, private to this repository | Refilled automatically by any run that restores the inputs (see below) |
| 2 | **Release asset**, tag `build-inputs`, asset `vt3-translated-inputs.tar.gz` | the same two paths, plus a `sha256sum` line in the run log | A release in this private repository (up to 2 GB per asset, no 100 MB git limit) | `Run workflow` with **publish_inputs** ticked, from a run that has `build/game` |
| 3 | **`prebuilt/`** in the repository | `libvt3android.so` (14.7 MB), `shaders.pack` (5.2 MB), `SHA256SUMS.txt` | Git, private repository only | Committed once from a working build (`prebuilt/README.md`) |

All three are game-derived. All three are only legitimate while the repository is private, which is
why the workflow's first job refuses to run on a public repository instead of quietly publishing an
APK that contains translated Sega code.

## 2. What the workflow does, including cache misses

```
cache restore ──hit──> build/game + shaders.pack ──> mode "translated" ──> compile everything
      │
      └──miss──> release asset download ──found──> build/game + shaders.pack ──> mode "translated"
                        │                              (then refill the cache)
                        └──not found──> prebuilt/ present? ──yes──> mode "prebuilt"
                                                     └──no───> fail, with the checklist
```

- A cache miss is normal, not an error: `actions/cache/restore` is allowed to find nothing.
- The cache is **not** storage. GitHub evicts caches after 90 idle days and may drop them earlier;
  that is exactly what store 2 is for. Both are consulted on every run, so the pipeline recovers by
  itself.
- With neither store, the run fails at the "Select the build inputs" step and names what is missing.
  It never builds an APK from a partial or wrong input: `build/game/image.bin` must hash to
  `52be3cda5edb9aa536394fed656e22270d688d74e86db8602fd39a7512fc80bf`, and `prebuilt/` must match its
  own `SHA256SUMS.txt` — and the library extracted from the finished APK is compared to that hash a
  second time.

## 3. What each mode rebuilds

| | `translated` (stores 1 and 2) | `prebuilt` (store 3) |
| --- | --- | --- |
| Native game + runtime (`runtime/`, `tools/recomp/`, the translated C) | **recompiled and shipped** | not recompiled; the preserved library ships |
| On-device installer (`android/game/jni/`) | recompiled | recompiled |
| Java UI, diagnostics | recompiled | recompiled |
| Resources, manifest, versionCode | rebuilt | rebuilt |
| Native and Java diagnostics tests | run | run (they build the repository's own sources) |
| Native changes from a new commit reach the APK | yes | **no** |

`prebuilt` mode is what keeps the pipeline alive while the translated source is unavailable: UI,
diagnostics, installer, resources and manifest changes all ship, from a phone. Changes under
`runtime/` still compile and pass their tests in that mode, but the APK carries the last preserved
library — `BUILD_INFO.txt` states it plainly (`native code in this build:`).

## 4. Refreshing the stores

To move from `prebuilt` to `translated`, the translation has to be produced once on a machine that
holds the executable (any computer, one time — not necessarily the phone owner's):

```sh
python build.py inputs --exe VT3.exe        # -> build/game/{vt3.c,image.bin} + build/shaders.pack
tar -czf vt3-translated-inputs.tar.gz build/game build/shaders.pack
```

then either attach that archive to a release tagged `build-inputs` (GitHub web UI, **Releases → the
tag → Attach binaries**), or commit the two paths to the private repository and run the workflow with
**publish_inputs** ticked. The next run then restores them, refills the cache, and switches to
`translated` mode by itself. Nothing in the workflow has to change.

To refresh `prebuilt/` after a native change, take the library from a `translated` run's APK:

```sh
unzip -p VT3-Android.apk lib/arm64-v8a/libvt3android.so > prebuilt/libvt3android.so
cd prebuilt && sha256sum libvt3android.so shaders.pack > SHA256SUMS.txt
```

## 5. Privacy

- Nothing game-derived is ever fetched from a public URL, and no executable is downloaded by any job.
- The release asset, the cache and `prebuilt/` are readable only by people who can read this
  repository, and the workflow refuses to run while it is public.
- Artifacts are checked before upload: the APK plus `SHA256SUMS.txt` and `BUILD_INFO.txt`, nothing
  else, and the job fails if an executable, image, archive, C source or library would slip in.
- The keystore and its passwords live in the repository secret `ANDROID_DEBUG_KEYSTORE_B64`, never in
  git and never in an artifact.
