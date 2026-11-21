# prebuilt/ — the compiled game library, for builds that have no game file

`libvt3android.so` is the ARM64 library (translated game + runtime, already stripped) that goes into
the APK, and `shaders.pack` is the SPIR-V shader pack bundled as its asset. Both are generated from
the player's own `VT3.exe`; neither is ever committed to a public repository, and neither can be
rebuilt without that executable.

They are here so GitHub Actions can keep producing debug APKs from the phone alone. The workflow
uses them only when it has no translated C source (`build/game/vt3.c` + `image.bin`), and then it
rebuilds everything else — the Java UI, resources, manifest and the on-device installer — around
this library. In that mode native changes under `runtime/` are **not** recompiled; see
[`docs/BUILD_INPUTS.md`](../docs/BUILD_INPUTS.md) for the three input stores and how to move to the
mode that does recompile.

## What belongs here

| File | Size | SHA-256 |
| --- | --- | --- |
| `libvt3android.so` | 14,764,976 bytes | `3d4b2d02249e86efaaefe83d2e25b2c0c2149257b1fa116e36979ff7135697b6` |
| `shaders.pack` | 5,217,428 bytes | `cabcc90894906978dff7a79f75acac7d81ad0ec33b6cf7db71bd75aadfd593a6` |
| `SHA256SUMS.txt` | — | `sha256sum libvt3android.so shaders.pack > SHA256SUMS.txt` |

The workflow verifies both against `SHA256SUMS.txt` and again against the library inside the signed
APK, so a truncated or swapped file fails the run instead of shipping.

## How to fill it

Either take them from a debug APK that already works:

```sh
unzip -p VT3-Android.apk lib/arm64-v8a/libvt3android.so > prebuilt/libvt3android.so
unzip -p VT3-Android.apk assets/shaders.pack             > prebuilt/shaders.pack
```

or copy them from a full local build (`build/android/libvt3android.so`, `build/shaders.pack`), then

```sh
cd prebuilt && sha256sum libvt3android.so shaders.pack > SHA256SUMS.txt
```

Commit them **only in a private repository**: this is machine code translated from Virtua Tennis 3.

## Why they are not committed yet

The repository is public today, so committing this library would publish Sega-derived code. The
workflow refuses to build from a public repository for the same reason. Once the repository is
private (`Settings → General → Danger Zone → Change repository visibility → Private`), commit these
two files and every push to `vt3android` produces a downloadable debug APK again.
