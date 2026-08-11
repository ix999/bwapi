# OpenBW Replays — Android

A Brood War replay viewer that runs entirely on the phone. No server, no
streaming, no account: the OpenBW engine is compiled into the app and simulates
the replay locally, exactly as the desktop `gfxtest` viewer does.

Playback is entirely local and **nothing is ever uploaded**. The app makes one
kind of outbound request, and only if you turn it on: downloading replays from
a private repository, so games produced by a cloud machine reach the phone. Off
by default, it makes no network requests at all.

## Status

**The APK builds**, on CI, for `arm64-v8a`, `armeabi-v7a` and `x86_64` — about
11 MB for a debug build. Download it from the Actions tab; see
[Building](#building).

The engine integration is verified too: the playback core loads retail 1.16.1
MPQs, parses real replays, plays, seeks, rewinds and pauses correctly, across a
range of maps and replay lengths (5k–45k frames). Peak memory stays flat at
~86 MB whether a run simulates half the replay or 95% of it, which is the
snapshot cap doing its job.

What is **not** verified is the app running on a device. Compiling proves the
Kotlin, the JNI signatures and the native build all line up; it says nothing
about whether the surface renders, touches land where they should, or the
importers behave against a real SAF provider. That needs a phone.

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
> under `android/`. This is the path the APK is currently built through.

### Don't trim SDL's subsystems

It is tempting to switch off the SDL subsystems the viewer never uses, but
SDL's Android build does not support it. `src/core/android/SDL_android.c` calls
`Android_AddHaptic`/`Android_RemoveHaptic` unguarded, so `SDL_HAPTIC=OFF` fails
to compile. `SDL_HIDAPI=OFF` is worse: it compiles, then fails at runtime,
because SDL's `HIDDeviceManager.java` is built from the same submodule and
binds to native methods that no longer exist. Only the shared/static/test
switches are safe to set.

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

## Getting replays onto the phone

Besides picking files by hand, the app can watch **one folder** and import any
new `.rep` it finds, on launch. Tap **Sync from a folder…**, pick the folder,
and that grant persists across reboots. Nothing runs in the background — a
replay viewer does not need to sync while it is closed, and this way it costs
no battery.

Pair it with whatever already syncs that folder from your desktop. The app does
no networking on this path, so the sync tool is entirely your choice.

**Why there is no Google Drive integration.** It would mean an OAuth flow and
Play Services for no benefit over a plain folder, which works with any
provider.

**Which sync tool.** Be aware that Google Drive is the weakest option here, not
the obvious one: the Drive Android app does not reliably expose a folder to
`ACTION_OPEN_DOCUMENT_TREE`, so it may not be selectable as a watched folder at
all. In rough order of how well they work:

| Tool | How it behaves |
| --- | --- |
| **Syncthing** | Best fit. Writes real files to real local storage, no account, syncs over your LAN. Point it at the Mac replay folder. |
| Dropbox / Nextcloud | Provide proper document providers; selectable as a watched folder. |
| Google Drive | May not be pickable as a tree. Workaround: save replays to `Downloads` from the Drive app and watch `Downloads` instead. |

On the Mac side, sync the retail replay folder — see the repo's
[CLAUDE.md](../CLAUDE.md#where-generated-replays-go) for the exact path.

### From a cloud machine

A folder on this phone is no use for replays produced by a cloud session
running the bot: there is no desktop in the loop to sync them. For that, the
app fetches directly from a **private GitHub repository**.

Tap **Cloud replays…**, enable it, and give it the repo, branch, folder and a
fine-grained token with read-only *Contents* access. The token is stored on the
device and never shipped in the APK.

By default it searches **every branch**. A cloud session's push protection
normally confines it to its own working branch, so watching a single branch
would mean retyping a branch name here every time a session reported one.
Branches sharing a commit are walked once, and a replay present on several
branches is downloaded once — matching is on the blob hash, not the path.

Listing uses the git trees API recursively, so the layout does not matter: it
finds the hash-sharded `replays/library/` corpus as well as flat files.
Downloads go through the contents API with a raw `Accept` header, because
`raw.githubusercontent.com` does not reliably honour a token on a private repo.

The same connection can fetch the **game data**. If the repo holds the three
mpqs, **Download from repo** on the setup card pulls whichever are missing,
instead of moving ~90 MB onto the phone by hand. It is a button rather than
something automatic, given the size, and the archives stay in the private repo
rather than being redistributed.

This is the app's **only** outbound traffic, it is off until enabled, and
nothing is ever uploaded. Replays belong in the private bot repo rather than in
this public one — they expose build orders.

## Starting up

The app opens the **newest replay** and starts playing immediately, so
launching it puts you in a game rather than a file list. Autoplay happens only
on a cold start; backing out of the viewer lands on the library, which would
otherwise be unreachable.

Inside the viewer, **Replays** in the top bar lists the library and switches
replay in place. The engine rebinds without reloading the mpqs or the image
data, so the swap is quick, and a paused viewer stays paused across it.

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
