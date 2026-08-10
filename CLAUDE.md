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

Two routes, depending on where the replay was produced.

**From this Mac** — the app watches one folder through the Storage Access
Framework and imports new `.rep` files on launch. Sync the replay folder above
with whatever tool you like, then tap **Sync from a folder…** in the app.

**From a cloud session** — there is no Mac in the loop, so commit the replay to
the private bot repo and let the phone fetch it:

```
repo:   ix999/starcraft-broodwar-bot     (private)
branch: with-assets                      (the branch carrying gitignored assets)
path:   replays/                         (searched recursively)
```

Write cloud-generated replays as `replays/<descriptive-name>.rep` on that
branch. The existing `replays/library/<shard>/<hash>.rep` corpus is picked up
too, since the app lists the tree recursively — but content hashes make poor
entries in the phone's picker, so name new ones properly.

**Never put replays in `ix999/bwapi`.** That repo is public, and replays expose
build orders.

## Building

- Android app: `cd android && ./gradlew assembleDebug`. Needs the Android
  SDK/NDK; CI (`.github/workflows/android.yml`) builds it on a GitHub runner if
  your environment cannot reach `dl.google.com`.
- Engine core without Android: `cmake -S tools/native-test -B build-native
  -DOPENBW_DIR=android/third_party/openbw && cmake --build build-native`. Run it
  against a directory of mpqs and a replay to exercise playback for real.
- Submodules are required: `git submodule update --init --recursive`.
