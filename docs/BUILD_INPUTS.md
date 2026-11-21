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
| 3 | **Private inputs repository**, `typhon-op/vt3-build-inputs` | `libvt3android.so` (14.7 MB), `shaders.pack` (5.2 MB), `SHA256SUMS.txt`, `BUILD_INFO.txt` | A separate **private** GitHub repository | Pushed once from a workspace that holds a working build; CI reads it with a read-only token |

All three are game-derived. Stores 1 and 2 only exist while this repository is private, and store
3 is a private repository of its own, which is why the compiled library never has to enter this
repository's history: the workflow's first job refuses to build here while it is public.

## 2. What the workflow does, including cache misses

```
cache restore ──hit──> build/game + shaders.pack ──> mode "translated" ──> compile everything
      │
      └──miss──> release asset download ──found──> build/game + shaders.pack ──> mode "translated"
                        │                              (then refill the cache)
                        └──not found──> private inputs repository has the library? ──yes──> mode "prebuilt"
                                                     └──no───> fail, with the checklist
```

- A cache miss is normal, not an error: `actions/cache/restore` is allowed to find nothing.
- The cache is **not** storage. GitHub evicts caches after 90 idle days and may drop them earlier;
  that is exactly what store 2 is for. Both are consulted on every run, so the pipeline recovers by
  itself.
- With neither store, the run fails at the "Select the build inputs" step and names what is missing.
  It never builds an APK from a partial or wrong input: `build/game/image.bin` must hash to
  `52be3cda5edb9aa536394fed656e22270d688d74e86db8602fd39a7512fc80bf`, and the library from the inputs
  repository must match that repository's `SHA256SUMS.txt` — and the library extracted from the
  finished APK is compared to that hash a second time.

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

To refresh store 3 after a native change, take the library from a `translated` run's APK and push
it to the private inputs repository:

```sh
unzip -p VT3-Android.apk lib/arm64-v8a/libvt3android.so > libvt3android.so
unzip -p VT3-Android.apk assets/shaders.pack              > shaders.pack
sha256sum libvt3android.so shaders.pack > SHA256SUMS.txt
git add libvt3android.so shaders.pack SHA256SUMS.txt BUILD_INFO.txt
git commit -m "Refresh the pinned library" && git push
```

## 5. Reading the private repository from CI

The workflow checks `typhon-op/vt3-build-inputs` out with `actions/checkout` and the repository
secret `BUILD_INPUTS_TOKEN`, at path `ci-inputs`, and verifies it with `sha256sum -c` before the
build. That token is a fine-grained personal access token with **Contents: Read-only** on that one
repository and nothing else; it is never written into a file, an artifact or a log, and the checkout
runs with `persist-credentials: false`. Without the secret (or if it expires) the checkout is skipped
with a notice and the run stops at "Select the build inputs" naming the missing store — it never
builds from an unverified library.

This repository needs no other access: it never writes to the inputs repository, and the inputs
repository never writes to this one.

## 6. Privacy

- Nothing game-derived is ever fetched from a public URL, and no executable is downloaded by any job.
- The release asset and the cache are readable only by people who can read this repository; the
  pinned library lives in the private `vt3-build-inputs` repository; and the workflow refuses to run
  while this repository is public.
- Artifacts are checked before upload: the APK plus `SHA256SUMS.txt` and `BUILD_INFO.txt`, nothing
  else, and the job fails if an executable, image, archive, C source or library would slip in.
- The keystore and its passwords live in the repository secret `ANDROID_DEBUG_KEYSTORE_B64`, never in
  git and never in an artifact.
