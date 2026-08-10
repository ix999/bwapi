# Working in this repo

A fork of BWAPI for OpenBW, plus `android/` — an on-device Brood War replay
viewer. See [android/README.md](android/README.md) for the app.

## Where generated replays go

When asked to create replays, write them to the retail replay-watch folder on
the Mac, so StarCraft and the phone both pick them up without a second copy
step:

```
~/Downloads/sc1161/maps/replays
```

Related paths, for reference:

| What | Path |
| --- | --- |
| Retail StarCraft 1.16.1 install | `~/Downloads/sc1161` |
| **Replay destination** | `~/Downloads/sc1161/maps/replays` |
| Retail automatic replays | `~/Downloads/sc1161/maps/replays/Autoreplay` |
| OpenBW sim replay output | `~/WebstormProjects/starcraft-broodwar-bot/build/replays` |

Notes:

- Replay-*generating* sims live in the `starcraft-broodwar-bot` repo, which
  writes to `build/replays` by default. Copy or symlink finished replays into
  the retail folder above; don't leave them only in `build/`, since that is
  wiped by a clean build.
- Give replays descriptive filenames — `matchup-map-date.rep` beats a content
  hash. The name is what shows in the phone app's picker.
- **Never commit replays or game data.** `.rep`, `.mpq`, `.scm` and `.scx` are
  copyrighted or derived from copyrighted assets. They are a local dependency
  only.

## Getting replays onto the phone

The app has no network permission by design, so it never fetches anything
itself. Instead it can watch one folder and import new `.rep` files on launch,
through the Storage Access Framework.

Set the Mac side up once so the replay folder above syncs to the phone, then in
the app tap **Sync from a folder…** and pick the synced folder. After that,
every replay written to the Mac folder appears in the app the next time it is
opened.

For which sync tool to use, and why the app does not integrate with Google
Drive directly, see
[android/README.md](android/README.md#getting-replays-onto-the-phone).

## Building

- Android app: `cd android && ./gradlew assembleDebug`. Needs the Android
  SDK/NDK; CI (`.github/workflows/android.yml`) builds it on a GitHub runner if
  your environment cannot reach `dl.google.com`.
- Engine core without Android: `cmake -S tools/native-test -B build-native
  -DOPENBW_DIR=android/third_party/openbw && cmake --build build-native`. Run it
  against a directory of mpqs and a replay to exercise playback for real.
- Submodules are required: `git submodule update --init --recursive`.
