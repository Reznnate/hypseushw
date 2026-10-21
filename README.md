# Daphne (HypseusHW) libretro core

A 64-bit libretro core, with sound, that plays the same data as `daphne.exe`.
It also comes as a standalone Android app.
Built and tested on Windows x64 and Android arm64 (RetroArch and the app), and on Linux x64 with the test host (Daphne and Singe 1 games; Singe 2's MP4 video has no decoder on Linux yet).
No BIOS or system files are needed.
The engine is [Hypseus Singe](https://github.com/DirtBagXon/hypseus-singe), vendored in `core/`.
Libretro changes are kept under `#ifdef __LIBRETRO__`.
It also plays Singe games, both Singe 1 (`daphne.exe singe`) and Singe 2 (`Singe.exe`, with MP4 video played as it is, no conversion).

## Content

Launch either of these:

- `framefile/<game>.txt`. This is what HyperSpin passes.
- `roms/<game>.zip`.
- A RocketLauncher batch file (`<game>.bat`) that runs `daphne.exe` or `Singe.exe`.

The Daphne folder layout is the usual one: `roms/`, `vldp/` and `framefile/`.
Existing Daphne `.dat` frame indexes are read but never rewritten.
When an index is missing or out of date, the core builds its own under
`<saves>/daphne/cache/`.
The core never writes into the Daphne folder.

A game that cannot start, for example because its video file is missing, shows the error on a black screen for 5 seconds and then quits, as a game that ends does.
RetroArch for Android hangs or crashes when a load it was started with fails, and launchers such as HyperSpin wait for RetroArch to exit either way.
The engine never opens dialogs of its own.

### Batch games (Singe)

RocketLauncher's *Daphne Batch* emulator runs `<game>.bat` instead of `daphne.exe`, which is how Singe games are usually set up.
The core does the same: for `framefile/<game>.txt` it looks for `<game>.bat` in the folders beside `framefile/` (such as `_New Version` or `Daphne2`), and when there is one it runs that file's `daphne.exe` or `Singe.exe` command from the file's folder.
The windowing arguments in it (`-x`, `-y`, `-fullscreen`, `-opengl` and the like) are dropped.

For a Singe 2 game (`Singe.exe -v <video> <script>`), the video plays as the laserdisc: frame N is the Nth picture of the MP4 (or M4V or MKV), and its sound track is the laserdisc audio.
The platform decodes it: MediaCodec on Android, Media Foundation on Windows.
Both use the hardware decoder by default; with *CPU Fallback* on, Android uses its software decoder instead and Windows turns off Media Foundation's hardware decoding.
A search goes to the key frame before the target and decodes forward to it.
`-l` and `-e` (video and effects volume, in percent) carry over; the other `Singe.exe` options do not matter here.
Singe 2's own menu (`Menu.bat`) is not supported.
Files a Singe game writes (settings, high scores) go to `<saves>/daphne/ram/` instead, and the game reads them back from there first.
Zip-packed Singe games (`-zlua`) and Singe's music functions are not supported.

## Per-game settings

Each game gets the settings of the stock RocketLauncher Daphne module: DIP switches, laserdisc seek timing and scoreboard style.
These settings are in `g_presets` in `libretro/lr_core.cpp`.

To give a game extra engine arguments, add a `<game>.commands` file.
It holds arguments separated by spaces, for example `-bank 0 11011001 -bank 1 00101111`.
The core reads every one of these that exists, in this order, and a later file overrides an earlier one.
All of them override the preset.

1. `roms/`
2. the Daphne folder
3. `<system>/daphne/`

## Core options

| Option (key) | Default | Effect |
|---|---|---|
| Fast boot (`daphne_fastboot`) | On | Skips the ROM and RAM tests where the game supports it. |
| Prefer samples (`daphne_prefer_samples`) | On | Uses sound samples instead of emulated sound. |
| Blank searches (`daphne_blank_searches`) | On | Blanks the video during laserdisc searches. |
| Service and Test switches (`daphne_service_buttons`) | Off | Puts the operator's Service and Test switches on L3 and R3. Off by default, because a stick click can also carry Coin or Start, and Test reboots most games into their diagnostics. |
| CPU Fallback (`daphne_cpu_fallback`) | Off | *Off* renders the video on the GPU (Vulkan or OpenGL (ES) 3) and decodes it with the hardware decoder where there is one. *On* does both on the CPU: libmpeg2 for MPEG-2 video, Android's software decoder for Singe 2's H.264 and HEVC, and Media Foundation without hardware decoding on Windows. Takes effect when a game is next loaded. |

The GPU renderer uses the same API as the frontend's video driver, which it learns from RetroArch:

- *vulkan* gets the Vulkan renderer, a compute shader (`libretro/shaders/compose.comp`).
- *gl* and *glcore* get the OpenGL renderer.
- Any other driver gets the CPU renderer automatically, for example Direct3D 11, the Windows default.

All three renderers produce identical pixels.
In Vulkan mode the core renders every frame, rather than reporting unchanged frames as repeats, because a repeated image could be overwritten while it is still on screen.
Uploads still happen only when the video or overlay changes.
The SPIR-V is committed (`compose.comp.inc`), and `libretro/shaders/build.sh` regenerates it with the NDK's `glslc`.

## Hardware MPEG-2 decoding (Android)

This section is about the MPEG-2 laserdisc video of Daphne and Singe 1 games; Singe 2's MP4 video is covered under *Batch games*.
With *CPU Fallback* off, VLDP decodes each MPEG-2 file with Android's MediaCodec hardware decoder, when all of these hold:

- The device has a hardware MPEG-2 decoder. A software-only MediaCodec decoder is not used, because libmpeg2 is at least as fast.
- The stream is progressive. Hardware decoders deinterlace interlaced pictures, which changes them, so interlaced streams stay on libmpeg2.
  For example Esh's Aurunmilla, Mach 3, Interstellar, Cobra Command and GP World.
- The decoder starts.

Otherwise that file is decoded with libmpeg2.
The log says which decoder each file gets (`VLDP: decoding ... with ...`).

The hardware path puts out the same pictures as libmpeg2, in the same order, so VLDP's frame numbers, searches and skips are unchanged.
After a reset (a search or skip), libmpeg2 still emits the B-pictures that depend on a picture before the reset point, and VLDP counts them, although it never shows them.
The hardware path doesn't decode those pictures, and puts a placeholder in their place.
Pictures are copied out of the decoder into VLDP's planes, so both renderers work unchanged.
`DAPHNE_LR_VERIFY_HWDEC=1` runs libmpeg2 alongside and checks every picture.

## Frame timing

Each `retro_run` should finish within one refresh, 16.7 ms.
The core enforces that in four places:

- **Game clock.** The game runs on a virtual clock that advances one frame per `retro_run`.
  If the engine has to wait for the decoder thread for more than 6 ms of a frame (for example to open a file or acknowledge a search), it gives the frame back.
  The game clock then holds still until the wait is over.
  The CPU cores never try to emulate more than 33 ms of missed time in one go.
- **Video decoder.** The MPEG decoder runs on its own thread.
  `retro_run` waits for it only when a picture is actually due, and never past 10 ms into the frame.
  If it's late, the previous picture stays up.
- **Network I/O.** During play, the game thread doesn't touch the network.
  The `.ogg` soundtrack is read on a background thread when the game loads.
  Opening video files is left to the decoder thread.
  The exception is switching to an alternate soundtrack, which is read directly.
- **Compositing.** With the GPU renderer, the YUV planes and the overlay are uploaded only when they change, and a shader converts and blends them into the frontend's framebuffer.
  With the CPU renderer, YUV to RGB conversion uses NEON on arm64, and each row is converted and blended with the overlay in one pass, split across two helper threads.
  Either way, frames that haven't changed are reported to the frontend as repeats.

With `DAPHNE_LR_PROFILE=1` set, a frame that still runs over budget is logged as `Slow frame N` with its breakdown.
When a game is closed, the log then gives the average and slowest frame, split by phase into wall time and CPU time.
It also gives the time spent waiting for a CPU core and the CPU used by the MPEG decoder and compose threads.
It also counts:

- frames shown while the decoder was still behind;
- frames the engine gave back;
- for searches and skips, how long the target frame took to decode (real time) and to appear (game time, including the seek delay the game is set up with).

CPU figures are 0 on Windows.

## Controls

| RetroPad | Arcade |
|---|---|
| D-pad | Joystick |
| B | Button 1 (sword) |
| A | Button 2 |
| Y | Button 3 |
| X / L / R | Skill 1 / 2 / 3 |
| Select | Coin 1 |
| Start | Start 1 |
| L2 | Coin 2 |
| R2 | Start 2 |
| L3 | Service (only with the *Service and Test switches* option) |
| R3 | Test (only with the *Service and Test switches* option) |

Singe gun games aim with RetroArch's lightgun (a lightgun, a mouse in lightgun mode or a touchscreen) or a mouse, with the trigger or left button as Button 1, the right button as Button 2 and the middle button as Button 3.
With a gamepad, the d-pad moves the crosshair.

## Supported games

Tested with the usual Daphne data set:

- **Daphne:** Dragon's Lair (and Dragon's Lair II), Space Ace, Astron Belt, Galaxy Ranger, Badlands, Bega's Battle, Cliff Hanger, Cobra Command, Esh's Aurunmilla, GP World, Interstellar, Mach 3, Road Blaster, Super Don Quix-ote, Thayer's Quest, Us vs. Them.
- **Singe 1:** Chantze's Stone, Conan, Daitarn 3, Dragon Trainer, Fire and Ice, Platoon, Puss 'N Boots, Samurai Jack, Starblazers.
- **Singe 2:** Asterix, Cliff Hanger HD, Dragon's Lair II (Singe 2 edition), Princess Mononoke, Sucker Punch, Time Gal, Titan A.E.

Other games the engine supports should work too.

## Known limitations

- No save states, rewind, run-ahead or netplay.
- Singe's music functions (SDL_mixer) are not supported; sound effects and the laserdisc audio are.
- Zip-packed Singe games (`-zlua`) and Singe 2's own menu (`Menu.bat`) are not supported.
- Singe 2 MP4 video needs Android or Windows; other platforms have no decoder for it yet.
- Content opened through an Android document URI (`content://`) cannot be played; the core needs a file path.

## Building

The dependencies are git submodules under `src/`, pinned in `UPSTREAM.md`.
Clone with `--recursive`, or run `tools/fetch-deps.sh` (the same as `git submodule update --init`).

Prerequisites:

- **Windows:** MSYS2, in a UCRT64 shell: `pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,zlib,sdl3}` (SDL3 only for `lr-host`).
  `mingw-w64-ucrt-x86_64-vulkan-headers` is optional: without it the Vulkan code uses the copy of the headers in `src/SDL3/src/video/khronos`.
- **Android:** the Android SDK with platform 35 and NDK 29.0.14206865, plus the SDK's CMake 3.22.1 and JDK 17 for the app.
  `android/local.properties` (not committed) gives the SDK location as `sdk.dir=...`, or set `ANDROID_HOME`.

Commands, from the repository root:

- **Windows core:**

  ```
  cmake -S libretro -B build/core-win -G Ninja -DCMAKE_BUILD_TYPE=Release
  ninja -C build/core-win
  ```

  This produces `build/core-win/hypseushw_libretro.dll`.

- **Android core (arm64):**

  ```
  cmake -S libretro -B build/core-android-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=24 -DANDROID_STL=c++_static
  ninja -C build/core-android-arm64
  ```

  This produces `hypseushw_libretro_android.so`.
  Keep the folder name `build/core-android-arm64`: the app and the test tools take the core from there.
  `ninja -C build/core-android-arm64 lr-host-cli` also builds a headless test frontend that runs from `adb shell`.

- **App:** `android/gradlew -p android assembleDebug` (or `assembleRelease`, signed when `android/keystore.properties` names a keystore; see `android/app/build.gradle`).
  It bundles the core from `build/core-android-arm64`.

- **lr-host** (desktop test frontend): `cmake -S host -B build/host-win -G Ninja && ninja -C build/host-win`.

- **Vulkan shader:** the SPIR-V is committed; after changing `libretro/shaders/compose.comp`, `libretro/shaders/build.sh` regenerates it.
  It finds `glslc` in the newest NDK, or takes `NDK=` (the NDK folder) or `GLSLC=` (the compiler) from the environment.

The top-level `Makefile` wraps the core builds (`make`, `make platform=android NDK=...`).
`.gitlab-ci.yml` builds the core with libretro's CI templates.
`libretro/hypseushw_libretro.info` is the RetroArch core info file; copy it to RetroArch's `info` folder with the core.


## Licence

GPL-3.0-or-later; see `LICENSE`.
The engine is Hypseus Singe by Matt Ownby, DirtBagXon and contributors (GPL-3.0); the libretro port, renderers, decoders and app are by the HypseusHW contributors.
Third-party components and their licences are listed in `THIRD_PARTY.md`.
Source code: https://github.com/Reznnate/hypseushw
