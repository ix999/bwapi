# OpenBW Replays — Android

A Brood War replay viewer that runs entirely on the phone. No server, no
streaming, no account: the OpenBW engine is compiled into the app and simulates
the replay locally, exactly as the desktop `gfxtest` viewer does.

The app declares **no `INTERNET` permission**. Replays and imported game
archives never leave the device.

## Status

The app source is complete, and the engine integration is verified: the
playback core loads retail 1.16.1 MPQs, parses real replays, plays, seeks,
rewinds and pauses correctly, across a range of maps and replay lengths
(5k–45k frames). Peak memory stays flat at ~86 MB whether a run simulates half
the replay or 95% of it, which is the snapshot cap doing its job.

It has **not been compiled into an APK yet** — that needs an Android SDK/NDK;
see [Building](#building). The Kotlin/Android layer is therefore unexercised;
what is proven is everything below the JNI boundary.

## Requirements

- Android Studio (or a standalone SDK) with:
  - Android SDK Platform 34
  - NDK (side-by-side) and CMake 3.22.1+
- JDK 17
- A phone running Android 5.0 (API 21) or newer
- Your own copy of StarCraft: Brood War 1.16.1

## Building

```sh
git submodule update --init --recursive   # SDL2 and openbw
cd android
./gradlew assembleDebug
```

The APK lands in `app/build/outputs/apk/debug/`.

Both native dependencies are pinned git submodules, so the build is
reproducible and works offline once cloned:

| Submodule | Pin |
| --- | --- |
| `third_party/SDL` | `release-2.30.9` |
| `third_party/openbw` | `8265ec44` |

> **Note on restricted networks.** The build resolves the Android Gradle Plugin
> and androidx from `dl.google.com`. In an environment that blocks that host the
> build fails at plugin resolution with
> `Plugin [id: 'com.android.application'] was not found`, before any code is
> compiled. Nothing in the project causes this; the host simply has to be
> reachable.
>
> If you cannot reach it, don't fight it — push the branch and let CI build.
> `.github/workflows/android.yml` builds the APK on a GitHub-hosted runner,
> which has the SDK and NDK preinstalled, and uploads it as a build artifact.
> Trigger it from the Actions tab (`workflow_dispatch`) or by pushing a change
> under `android/`.

## Game data

The app cannot ship Blizzard's data files. On first launch it asks for three
archives from a Brood War 1.16.1 installation:

```
Patch_rt.mpq   BrooDat.mpq   StarDat.mpq
```

They are imported through the system document picker — which grants access to
just those files, so the app needs no storage permission — validated for an MPQ
header, and copied into app-private storage. openbw opens them by exact name and
Android's filesystem is case-sensitive, so imports are always stored under the
capitalisation above regardless of how the source files were named.

Replays are imported the same way and copied into the library, because a SAF
content URI is not something native code can `fopen()`.

## How it fits together

```
MainActivity ────────── library: game-data setup, replay list
     │ starts
ViewerActivity ──────── extends SDLActivity; supplies libraries + argv,
     │                  lays a transport bar over SDL's surface
     │ JNI (NativeBridge)
libbwreplay.so
     ├── bwreplay_jni.cpp ──── queues commands, reads status snapshots
     ├── android_main.cpp ──── SDL_main: reads argv, runs the tick loop
     ├── core/bwreplay_core ── engine driver (shared with the desktop harness)
     └── openbw ui/sdl2.cpp ── openbw's software renderer
```

The engine runs on SDL's thread. The UI thread never touches it directly: it
queues commands that are applied at the top of the next tick, and reads a status
snapshot the engine publishes under its own lock. So the transport bar can never
block on a frame being rendered.

Touches outside the transport bar fall through to SDL, which turns them into
mouse events. That gives openbw's own in-game UI — minimap, unit selection,
screen dragging — for free rather than reimplementing it.

### Seeking

Playback keeps periodic full copies of the simulation state so it can seek
backwards; a rewind restores the nearest earlier snapshot and re-simulates
forward. Unlike `gfxtest.cpp`, which keeps every snapshot, the core caps how
many it holds: on reaching the cap the interval doubles and off-interval
snapshots are dropped. That bounds memory on a phone while keeping the
snapshots evenly spread, at the cost of a longer worst-case re-simulation.

## Testing the engine without Android

The core in `app/src/main/cpp/core` is platform independent, so it can be built
and exercised on a desktop against system SDL2:

```sh
cmake -S tools/native-test -B build-native -DOPENBW_DIR=android/third_party/openbw
cmake --build build-native
SDL_VIDEODRIVER=dummy ./build-native/bwreplay-native-test <dir-with-mpqs> <replay.rep>
```

It plays, seeks forward, rewinds, checks that pausing actually stops the
simulation, changes speed, and finally seeks to 95% — which simulates almost
the whole replay and so forces the snapshot cap to engage repeatedly. The
Android build compiles the same sources with the same flags.

Expected output against a real replay:

```
map: The Fortress 1.1
end frame: 21981
after seek 50%     frame=10993/21981 paused=0 speed=1.00 done=0
after rewind 10%   frame=2202/21981 paused=0 speed=1.00 done=0
after seek 95%     frame=20881/21981 paused=1 speed=8.00 done=0
OK
```

## Known limitations

- **No sound.** openbw routes audio through SDL_mixer, which is not built here,
  so `native_sound` compiles to stubs.
- **Landscape only.** SDL recreates its surface on rotation, which would restart
  playback.
- **No pinch-zoom yet.** Zoom is on the transport bar; pinch would need SDL
  touch events handled alongside the synthesised mouse events.
- **The Android layer is untested on a device.** The engine below JNI is
  verified; the activities, overlay and importers are not — see
  [Status](#status).
