#!/usr/bin/env bash
# fetch-sc-data.sh — populate an sc-data/ directory (retail 1.16.1 MPQs + the SSCAIT map
# pool) from the owner's private Google Drive snapshot, per MANIFEST.tsv. The game assets
# are copyrighted and are never committed to any repository; this script exists so a fresh
# cloud/CI checkout can pull them from storage the owner controls.
#
# Sources, in order:
#   1. SB_SC_DATA_SRC=<dir>  — copy from a local directory (e.g. a Drive-for-desktop mount
#      or an existing installation). No network needed.
#   2. SB_SC_DATA_GIT_URL=<url> [SB_SC_DATA_GIT_REF=sc-data-assets] — shallow-clone a
#      (private) git branch holding the assets and copy from it. Use for a private repo
#      the session already has credentials for; never point this at a public repo.
#   3. Google Drive download by file ID. Requires the Drive files to be link-shared
#      ("anyone with the link") by their owner; IDs alone grant nothing otherwise.
#
# Usage: fetch-sc-data.sh [DEST_DIR]   (default DEST_DIR: ./sc-data)
# Every file is size-verified against the manifest; sha256 is verified when the manifest
# has one, otherwise the computed hash is printed so it can be pinned.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
manifest="$here/MANIFEST.tsv"
dest="${1:-./sc-data}"
src_dir="${SB_SC_DATA_SRC:-}"

git_dir=""
if [ -n "${SB_SC_DATA_GIT_URL:-}" ]; then
  git_dir="$(mktemp -d)/sc-data-git"
  if ! git clone --quiet --depth 1 -b "${SB_SC_DATA_GIT_REF:-sc-data-assets}" \
      "$SB_SC_DATA_GIT_URL" "$git_dir"; then
    echo "warn: could not clone $SB_SC_DATA_GIT_URL — falling back to other sources" >&2
    git_dir=""
  fi
fi

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

fetch_drive() {
  local id="$1" out="$2"
  local url="https://drive.usercontent.google.com/download?id=${id}&export=download&confirm=t"
  if command -v gdown >/dev/null 2>&1; then
    gdown --quiet --id "$id" -O "$out" || return 1
  else
    curl -fsSL "$url" -o "$out" || return 1
  fi
  # An HTML page instead of file content means the file is not link-shared (or the ID moved).
  if head -c 1 "$out" | grep -q '<'; then
    rm -f "$out"
    return 1
  fi
}

fail=0
while IFS=$'\t' read -r path id size hash; do
  case "$path" in ''|'#'*) continue ;; esac
  out="$dest/$path"
  mkdir -p "$(dirname "$out")"

  if [ -f "$out" ] && [ "$(wc -c < "$out" | tr -d ' ')" = "$size" ]; then
    echo "ok (exists)  $path"
  elif [ -n "$src_dir" ] && [ -f "$src_dir/$path" ]; then
    cp "$src_dir/$path" "$out"
    echo "copied       $path"
  elif [ -n "$git_dir" ] && [ -f "$git_dir/$path" ]; then
    cp "$git_dir/$path" "$out"
    echo "from-git     $path"
  elif fetch_drive "$id" "$out"; then
    echo "downloaded   $path"
  else
    echo "FAIL         $path — not in SB_SC_DATA_SRC and Drive download failed" >&2
    echo "             (is the file link-shared? id=$id)" >&2
    fail=1
    continue
  fi

  got_size="$(wc -c < "$out" | tr -d ' ')"
  if [ "$got_size" != "$size" ]; then
    echo "FAIL         $path — size $got_size, manifest says $size" >&2
    fail=1
    continue
  fi
  got_hash="$(sha256 "$out")"
  if [ "$hash" != "-" ] && [ -n "$hash" ]; then
    if [ "$got_hash" != "$hash" ]; then
      echo "FAIL         $path — sha256 mismatch" >&2
      fail=1
    fi
  else
    echo "             $path sha256=$got_hash (unpinned — add to MANIFEST.tsv)"
  fi
done < "$manifest"

[ "$fail" = 0 ] || { echo "fetch-sc-data: one or more files failed" >&2; exit 1; }
echo "fetch-sc-data: complete → $dest (set OPENBW_MPQ_PATH=$dest)"
