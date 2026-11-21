# Virtua Tennis 3 Port

<p align="center">
  <img src="assets/icon/vt3_logo_icon_1024.png" alt="Virtua Tennis 3 port icon" width="160">
</p>

<p align="center">
  <img src="docs/screenshot.png" alt="A match running on the Retroid Pocket" width="100%">
</p>

The PC release of Virtua Tennis 3 (SEGA, 2007) running as native code on Android and Apple Silicon
Macs through static recompilation. This is not an emulator and nothing runs under Wine.

It is experimental. It is playable on the Retroid Pocket (Snapdragon 8 Gen 2).
Vulkan is the default renderer on both platforms, with OpenGL as the fallback on the Mac.

There are no downloads. You build the game yourself from your own copy: the build translates your
`VT3.exe` on your computer, so this repository never holds any of the game's code.

## What you need

- A disc image of the **European PC DVD** (English, French, German, Spanish, Italian), e.g.
  `Virtua Tennis 3 (Europe) (En,Fr,De,Es,It).iso`

  | File | Size | SHA-256 |
  | --- | --- | --- |
  | European PC DVD image | 2,623,963,136 bytes | `10862f2a0c35cd0673b406331494f2482d107a4efab52aa6b7725a8c346e6165` |

- The game's English v1.0.1 `VT3.exe` without disc protection (the bare file or a `.zip` holding it).
  The build checks it and stops on any other version.

Only the European PC release has been tested. The USA PC release has not.

## Building

`build.py` does everything: it translates your `VT3.exe`, builds the shader pack, compiles the
runtime and packages the result.

```bash
python build.py android --exe path/to/VT3.exe                     # build/android/VT3-Android.apk
python build.py macos --exe path/to/VT3.exe --iso path/to/VT3.iso  # build/macos/VT3.app (macOS only)
```

Compiling the translated game takes 5 to 15 minutes. The Android build runs on macOS, Linux and
Windows; the desktop build is macOS only.

### Tools for every platform

- Python 3.10+ with `pip install pefile capstone`
- CMake 3.22+, Ninja, and `glslangValidator` (from glslang or the Vulkan SDK)
- A host C compiler (`cc`, or set `CC`)
- For Android: JDK 17+, and the Android SDK with `platform-tools`, `platforms;android-35`,
  `build-tools;36.1.0` and an NDK (tested with r30). Set `ANDROID_HOME`, `ANDROID_NDK_HOME` and `JAVA_HOME`.

### macOS (Apple Silicon)

```bash
brew install python cmake ninja glslang openjdk@21 android-commandlinetools android-ndk
brew install sdl3 molten-vk vulkan-loader vulkan-headers   # only for the desktop build
sdkmanager "platform-tools" "platforms;android-35" "build-tools;36.1.0"
pip3 install pefile capstone
```

Homebrew's default paths are used when the environment variables are not set. The desktop build
unpacks the disc once into `~/Library/Application Support/VT3/data2` (about 4 GB).

### Linux (Debian or Ubuntu)

```bash
sudo apt install python3-pip cmake ninja-build glslang-tools openjdk-21-jdk clang
pip install pefile capstone
# Android command-line tools: https://developer.android.com/studio#command-tools
sdkmanager "platform-tools" "platforms;android-35" "build-tools;36.1.0" "ndk;30.0.16248370"
export ANDROID_HOME=~/Android/Sdk ANDROID_NDK_HOME=~/Android/Sdk/ndk/30.0.16248370
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
```

### Windows

Install Python, CMake, Ninja, LLVM (for `clang`), a JDK 21, the Vulkan SDK (for
`glslangValidator`) and Android Studio or its command-line tools, e.g. with `winget`. Then, in PowerShell:

```powershell
pip install pefile capstone
sdkmanager "platform-tools" "platforms;android-35" "build-tools;36.1.0" "ndk;30.0.16248370"
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
$env:ANDROID_NDK_HOME = "$env:ANDROID_HOME\ndk\30.0.16248370"
$env:JAVA_HOME = "C:\Program Files\Microsoft\jdk-21"   # wherever your JDK is
$env:CC = "clang"
python build.py android --exe C:\path\to\VT3.exe
```

The Linux and Windows steps have not been tested yet. Please report what breaks.

## Installing on Android

1. Install `build/android/VT3-Android.apk` (for example with `adb install`).
2. Copy your disc image and `VT3.exe` (or its zip) onto the device.
3. Open **VT3**, tap **Select PC disc image**, then **Select VT3.exe**, then **Install**.
   Unpacking takes about a minute. Keep the app open.
4. The game starts. Later launches go straight into the game.

Requirements: Android 10 or newer, ARM64, Vulkan, and about 4.5 GB free. Controllers work, and
on-screen touch controls appear on phones. Uninstalling the app removes the unpacked game.

### Game files and storage access

The picker is all that is needed: whatever you choose is read through the system's document API and
copied into the app's own storage, which is the only place the game reads from. Nothing asks for a
broad storage permission to install.

The setup screen also offers two optional shortcuts, both behind Android's **All files access**
(`MANAGE_EXTERNAL_STORAGE`), which you grant yourself on a system settings screen the app opens for
you, and which the app re-checks every time you come back to it:

- **Use an unpacked game folder** — if you already have the PC install unpacked on the phone, the
game reads it where it lies instead of unpacking a 4.5 GB image.
- **Install folder** — puts `data2` on a folder of your choice instead of app storage. The path is
  recorded for the runtime, which still only ever opens plain paths.

Nothing breaks without that permission: the screen says it is optional, the picker path always
works, and a folder chosen with the picker is copied through real storage first, so no `content://`
URI is ever handed to the game.

**Google Play:** All files access is a restricted permission. Play's policy limits it to file
managers, backup/restore, antivirus, document management, on-device search, encryption and device
migration, and explicitly names user-picked files as a case for the Storage Access Framework instead
([policy](https://support.google.com/googleplay/android-developer/answer/10467955)). A game installer
is not on that list, so a Play build should ship without the `uses-permission` line and rely on the
picker; the app is designed to work fully that way. This port is side-loaded rather than published.

### Diagnostics

**Settings → Diagnostics** (also reachable from the setup screen) is where a problem gets recorded
and reported. The app keeps one plain-text log on the device — startup and shutdown, the game engine
and executable loading, installation and file checks, storage grants and custom paths, Vulkan and
frame times, audio state and recovery, touch/pad input, and the Android pause/resume/window
lifecycle — with every entry stamped, levelled (DEBUG/INFO/WARN/ERROR/FATAL), tagged with its
component and, where one exists, an error code and stack trace. The native engine and the Android
code write to the same file; nothing is uploaded.

The screen offers **View logs**, **Generate diagnostic report**, **Save report as TXT**, **Share
report**, **Open last saved report**, **Clear logs** and **View last error**, shows the log size,
app version, last report time and last error, and has a **detailed logging** switch for
troubleshooting (off by default, so normal play is not flooded).

The report is a real `VT3_Diagnostic_Report.txt` file you pick a folder and name for,
with sections for application info, the install and its file verification, graphics and frame
statistics, audio diagnostics, the Android lifecycle, the error history and a summary of detected
problems. It is saved through the Storage Access Framework, so it needs no storage permission;
after writing, the app reopens the file and checks it before saying it was saved. Sharing attaches
that same text file.

Logs are rotated with a size cap (2 MB per file, three older files kept, configurable), written off
the game and audio threads through a bounded queue, and a crash is preserved for the next launch
(Java uncaught exceptions, and a native signal line written straight to disk). A native crash that
kills the process before anything can be written cannot be recorded; the report says so rather than
pretending otherwise.

## How it works

1. `tools/recomp/recomp.py` statically translates the x86 code of the v1.0.1 `VT3.exe` into C.
   Clang compiles that C for ARM64 (Android) or Apple Silicon (macOS).
2. A Win32, DirectSound and Direct3D 9 shim in plain C (`runtime/shim`) stands in for Windows.
   Direct3D 9 draws go to a Vulkan renderer, or to OpenGL on the Mac. The game's shader model 2
   shaders are translated to GLSL, and to a prebuilt SPIR-V pack for Vulkan.
3. SDL3 supplies the window, controllers, touch and audio.
4. On Android, a small launcher reads the disc image through the system file picker. It unpacks the
   InstallShield cabinets on the device (with [unshield](https://github.com/twogood/unshield)) and
   builds the guest image from `VT3.exe`, checking it against a known SHA-256. Everything stays in the
   app's own storage.

The repository holds no retail executables, assets or generated game code.

Developer notes and the current state of the port are in [docs/HANDOFF.md](docs/HANDOFF.md).

## Current status

- Boots, menus, tournaments and matches play on Mac and Android
- Native Vulkan rendering on both, with OpenGL as the fallback on the Mac
- Widescreen (Hor+) fill
- Controllers on both platforms, with on-screen touch controls on Android

## More documentation

- [docs/recomp-design.md](docs/recomp-design.md): the recompiler
- [docs/d3d9-shaders.md](docs/d3d9-shaders.md): shader translation
- [docs/WIDESCREEN.md](docs/WIDESCREEN.md) and [docs/TOUCH_CONTROLS.md](docs/TOUCH_CONTROLS.md)
- [docs/formats.md](docs/formats.md): game data formats
- [docs/decisions.md](docs/decisions.md): decision log

## Legal

This repository contains no copyrighted game code or game data from Virtua Tennis 3: no
executables, assets, or code generated from them. A legitimately owned copy of the game is
required to play. Virtua Tennis 3 and all related trademarks and copyrights belong to SEGA
Corporation. This is an unofficial fan project and is not affiliated with or endorsed by SEGA.
