#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../openbw/bwgame.h"

// CachedAssetLoader (bot-stack half) — serves bwgame::global_init's data files from a persistent
// DECOMPRESSED cache instead of re-parsing/decompressing the MPQs on every process start. The
// harness half (tools/qa/qa_env.cpp, commit 6e3c37d) proved the pattern and the numbers: init
// 3.35s cold -> 0.06-0.14s hot, 956 files / ~15 MB, byte-identical (=verify gate). This half
// covers BWAPILauncher games — the boundary profile measured ~650M Ir of MPQ decompress per
// process (~25% of a short battery game) that this removes on warm starts.
//
// Env contract (shared with the harness half): OPENBW_ASSET_CACHE unset/1 = on, 0 = off,
// verify = load from MPQs AND byte-compare against the cache (mismatch aborts, exit 3).
// OPENBW_ASSET_CACHE_DIR = cache root; harnesses MUST export an ABSOLUTE path (bot games run in
// per-game scratch cwds — a relative default would fragment the cache; vs.sh exports
// $ROOT/build/asset-cache). Cache keyed by a fingerprint of the three MPQs' sizes+mtimes.
// NOTE: the harness half fingerprints via std::filesystem's file clock; this C++14 TU uses
// POSIX st_mtime — on platforms where the epochs differ the two halves keep separate (equally
// correct) cache directories. Unexpected layout or unwritable root -> silently uncached.
namespace BW {

struct CachedAssetLoader {
  bwgame::data_loading::data_files_loader<> inner;
  std::string dir;   // empty => disabled
  bool verify = false;

  void operator()(bwgame::a_vector<uint8_t>& dst, bwgame::a_string filename) {
    if (dir.empty()) { inner(dst, std::move(filename)); return; }
    std::string key(filename.begin(), filename.end());
    for (auto& ch : key) if (ch == '/' || ch == '\\') ch = '_';
    const std::string path = dir + "/" + key;
    if (!verify) {
      if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        dst.resize((size_t)len);
        const size_t got = len ? std::fread(dst.data(), 1, (size_t)len, f) : 0;
        std::fclose(f);
        if ((long)got == len) return;   // served decompressed
      }
    }
    inner(dst, filename);
    if (verify) {
      if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::string c((size_t)len, '\0');
        const size_t got = len ? std::fread(&c[0], 1, (size_t)len, f) : 0;
        std::fclose(f);
        if ((long)got != len || (size_t)len != dst.size() ||
            (len && std::memcmp(c.data(), dst.data(), (size_t)len) != 0)) {
          std::fprintf(stderr, "ASSET-CACHE VERIFY MISMATCH (bot stack): %s\n", path.c_str());
          std::exit(3);
        }
      }
      return;
    }
    const std::string tmp = path + ".tmp." + std::to_string((long)::getpid());
    if (FILE* o = std::fopen(tmp.c_str(), "wb")) {
      const size_t put = dst.empty() ? 0 : std::fwrite(dst.data(), 1, dst.size(), o);
      const bool ok = (std::fclose(o) == 0) && put == dst.size();
      if (ok) { if (::rename(tmp.c_str(), path.c_str()) != 0) ::unlink(tmp.c_str()); }
      else ::unlink(tmp.c_str());
    }
  }
};

inline CachedAssetLoader make_cached_asset_loader(const std::string& mpq_path) {
  CachedAssetLoader L{ bwgame::data_loading::data_files_directory<bwgame::data_loading::data_files_loader<>>(mpq_path.c_str()), std::string(), false };
  const char* env = std::getenv("OPENBW_ASSET_CACHE");
  if (env && std::strcmp(env, "0") == 0) return L;
  std::uint64_t fp = 1469598103934665603ull;
  auto mix = [&fp](std::uint64_t v) { fp ^= v; fp *= 1099511628211ull; };
  for (const char* n : { "Patch_rt.mpq", "BrooDat.mpq", "StarDat.mpq" }) {
    struct stat sb;
    const std::string mp = mpq_path + "/" + n;
    if (::stat(mp.c_str(), &sb) != 0) return L;   // unexpected layout: run uncached
    mix((std::uint64_t)sb.st_size);
    mix((std::uint64_t)sb.st_mtime);
  }
  char buf[24];
  std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)fp);
  const char* dir_env = std::getenv("OPENBW_ASSET_CACHE_DIR");
  std::string root = dir_env && *dir_env ? std::string(dir_env) : std::string("build/asset-cache");
  root += "/botstack-";   // POSIX-mtime keyspace — see epoch note above
  root += buf;
  ::mkdir(dir_env && *dir_env ? dir_env : "build", 0755);
  ::mkdir((dir_env && *dir_env ? std::string(dir_env) : std::string("build/asset-cache")).c_str(), 0755);
  if (::mkdir(root.c_str(), 0755) != 0) {
    struct stat sb;
    if (::stat(root.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) return L;   // unwritable: uncached
  }
  L.dir = root;
  L.verify = env && std::strcmp(env, "verify") == 0;
  return L;
}

}
