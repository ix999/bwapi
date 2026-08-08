# sc-data fetch helper

OpenBW needs the retail StarCraft 1.16.1 data files (`StarDat.mpq`, `BrooDat.mpq`,
`Patch_rt.mpq`) and, for bot work, the SSCAIT/BASIL map pool. Those files are
copyrighted game assets and are **never committed to this or any repository** —
they live only in storage the owner controls (a verified local installation and a
private Google Drive snapshot).

This directory makes them reachable from a fresh cloud or CI checkout without
putting the assets themselves in git:

- `MANIFEST.tsv` — the expected files: repo-relative path under `sc-data/`,
  Google Drive file ID in the owner's snapshot, byte size, and (when pinned)
  sha256.
- `fetch-sc-data.sh [DEST_DIR]` — populates `DEST_DIR` (default `./sc-data`)
  from either a local source directory (`SB_SC_DATA_SRC=<dir>`, e.g. a
  Drive-for-desktop mount) or by downloading the Drive IDs. Every file is
  size-verified; hashes are verified when pinned and printed for pinning when
  not.

Drive downloads only work if the owner has link-sharing enabled on the snapshot
files; the IDs by themselves grant no access. In managed cloud sessions the
environment's network policy must also allow `drive.google.com` /
`drive.usercontent.google.com` (configurable in the environment settings), or
the download path is blocked regardless of sharing. Claude sessions with the
Google Drive connector can instead resolve the same IDs through the connector,
which needs no link-sharing but rejects files over 10 MB — enough for every
map and `Patch_rt.mpq`, not for `StarDat.mpq`/`BrooDat.mpq`. Those two can
only come from a local source (`SB_SC_DATA_SRC`) or an unblocked download.

After fetching:

```sh
export OPENBW_MPQ_PATH=$PWD/sc-data
```
