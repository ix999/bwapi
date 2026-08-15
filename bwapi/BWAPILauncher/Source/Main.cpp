
#include "BWAPI/GameImpl.h"
#include "BW/BWData.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <memory>
#include <fstream>
#include <array>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <unistd.h>
#include <sys/time.h>

// ---- Dual-host mode (ENGINE_OPT_DUALHOST.md): OPENBW_DUAL_HOST=1 hosts BOTH bots in this
// process against ONE simulation — no lockstep peer, no IPC, no duplicate sim. Each bot runs
// on its own dispatch thread (thread_local BroodwarImpl + Broodwar bind per thread); the two
// updates are barriered strictly sequentially each frame, then the main thread steps the sim
// once. Env: BWAPI_CONFIG_AI__AI + BWAPI_CONFIG_AUTO_MENU__RACE for bot 0; GLITCHCORE_P2_AI (a
// DISTINCT FILE COPY of the module) + GLITCHCORE_P2_RACE for bot 1.

namespace BWAPI { void mirrorShareNextGame(); }   // UnitUpdate.cpp — serial-K per-game reset

namespace {

int race_from_env(const char* name, int fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  std::string s = v;
  s[0] = (char)std::toupper((unsigned char)s[0]);
  if (s == "Zerg") return 0;
  if (s == "Terran") return 1;
  if (s == "Protoss") return 2;
  return fallback;
}

struct bot_lane_t {
  std::thread th;
  std::mutex m;
  std::condition_variable cv;
  std::atomic<bool> go{false}, done{false};
  bool quit = false, finish = false;
  bool module_ok = true;
};

// SPIN-THEN-WAIT seam on the per-frame lane handshake (SB_LANE_SPIN=<iterations>, DEFAULT 0 =
// pure condvar). The baton passes sim -> bot0 -> sim -> bot1 -> sim every frame — 4 kernel wake
// round-trips — and a 2026-08-13 fleet sample showed threads parked in __psynch_cvwait 60-93%
// of wall. MEASURED ON THE M2 (2026-08-13): spinning is NEGATIVE in BOTH regimes there —
// saturated battery conc 9: cpu 93s -> 169s, wall 15.1s -> 23.9s (27 threads on 10 cores;
// spinners steal cores from computers, whose condvar waits the scheduler was already filling
// with other games' compute); solo: cpu 0.5 -> 1.1s, wall unchanged (the solo idle is
// startup I/O + lobby, not wake latency). Default stays 0 on the Mac. The seam is kept for the
// LADDER regime (one dual game, idle cores, different scheduler) — evaluate in the container
// before enabling there. Ordering: the signaller's plain fields (quit/finish/module_ok) are
// written before the release-store of the flag, so an acquire-load publishes them — the mutex
// is only needed on the condvar sleep path. Timing-only either way: dispatch order is
// unchanged, digests are unaffected (gated identical spin on/off).
inline int lane_spin_iters() {
  static const int iters = [] {
    const char* e = std::getenv("SB_LANE_SPIN");
    return e ? std::atoi(e) : 0;
  }();
  return iters;
}

inline bool lane_spin(std::atomic<bool>& flag) {
  for (int i = lane_spin_iters(); i-- > 0;) {
    if (flag.load(std::memory_order_acquire)) return true;
#if defined(__aarch64__)
    __asm__ __volatile__("isb");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#endif
  }
  return flag.load(std::memory_order_acquire);
}

void lane_signal_go(bot_lane_t& lane, bool quit = false, bool finish = false) {
  {
    std::lock_guard<std::mutex> lock(lane.m);
    lane.quit = quit;
    lane.finish = finish;
    lane.go.store(true, std::memory_order_release);
  }
  lane.cv.notify_all();
}

// ---- Per-viewer policy channel (per-side P1_POLICY/P2_POLICY in one process, NO bot changes) ----
//
// Two-process games apply P1_POLICY to process 1 and P2_POLICY to process 2 — each a space-separated
// list of KEY=VALUE env assignments (e.g. "GLITCHCORE_GENOME=/path GLITCHCORE_QA_FORCE_OPERATION=nuke"). In
// dual-host both bots share one process env, so the runner passes them as GLITCHCORE_P1_POLICY /
// GLITCHCORE_P2_POLICY and this applies the ACTIVE viewer's set to the real env immediately before that
// viewer's dispatch. Because per-frame dispatch is strictly sequential (bot 0 completes before bot 1
// starts) and each viewer is a distinct module image with its own statics, every getenv the bot
// makes during its dispatch returns its own value, and lazily-cached `static const … = getenv(…)`
// reads cache the correct per-viewer value. No bot code changes; the runner interface is unchanged.
//
// Keys present in EITHER side form the managed union: for the active viewer, each key is set to that
// side's value or UNSET if that side does not name it — so a key on one side never leaks to the
// other. Config only changes which commands a bot issues; it cannot affect sim determinism or a
// viewer's fog, so a symmetric game (equal or empty policies) is byte-identical to before, and a
// differentiated game matches two-process with the same per-side policies (gated by
// dual-equivalence.sh).
struct ViewerPolicies {
  std::array<std::map<std::string, std::string>, 2> side;
  std::set<std::string> keys;   // union of keys across both sides
  bool active = false;
};

ViewerPolicies parse_viewer_policies() {
  ViewerPolicies vp;
  const char* names[2] = { "GLITCHCORE_P1_POLICY", "GLITCHCORE_P2_POLICY" };
  for (int v = 0; v < 2; ++v) {
    const char* raw = std::getenv(names[v]);
    if (!raw || !*raw) continue;
    std::istringstream in(raw);
    std::string tok;
    while (in >> tok) {
      const auto eq = tok.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = tok.substr(0, eq);
      vp.side[v][key] = tok.substr(eq + 1);
      vp.keys.insert(key);
      vp.active = true;
    }
  }
  return vp;
}

void apply_viewer_policy(const ViewerPolicies& vp, int viewer) {
  if (!vp.active) return;
  for (const std::string& key : vp.keys) {
    const auto& m = vp.side[viewer];
    const auto it = m.find(key);
    if (it != m.end()) ::setenv(key.c_str(), it->second.c_str(), 1);
    else ::unsetenv(key.c_str());   // not named on this side — never leak the other side's value
  }
}

void lane_wait_done(bot_lane_t& lane) {
  if (!lane_spin(lane.done)) {
    std::unique_lock<std::mutex> lock(lane.m);
    lane.cv.wait(lock, [&] { return lane.done.load(std::memory_order_acquire); });
  }
  lane.done.store(false, std::memory_order_release);
}

int run_one_dual_game() {
    try {
      BW::GameOwner gameOwner;

      gameOwner.setPrintTextCallback([](const char* str) {
        std::string s;
        while (*str) {
          char c = *str++;
          if ((unsigned)c >= 0x20 || c == 9 || c == 10 || c == 13) s += c;
        }
        printf("%s\n", s.c_str());
      });

      BW::Game g0 = gameOwner.getGame(0);
      BW::Game g1 = gameOwner.getGame(1);

      const char* map = std::getenv("BWAPI_CONFIG_AUTO_MENU__MAP");
      if (!map || !*map) { printf("dual: BWAPI_CONFIG_AUTO_MENU__MAP not set\n"); return 1; }
      const char* p2ai = std::getenv("GLITCHCORE_P2_AI");
      if (!p2ai || !*p2ai) { printf("dual: GLITCHCORE_P2_AI not set (needs a distinct file copy)\n"); return 1; }
      int race1 = race_from_env("BWAPI_CONFIG_AUTO_MENU__RACE", 1);
      int race2 = race_from_env("GLITCHCORE_P2_RACE", 1);

      g0.setMapFileName(map);
      g0.setGameTypeMelee();

      bool race1_sent = false, race2_sent = false;
      // Seating matches the two-process reference exactly: both reference walkers request the
      // first open slot each lobby pump, occupies apply in uid-sorted client order, so the
      // lower-sorting uid wins slot 0 and the other lands slot 1. Uids derive from
      // (seed, client index), so WHICH client sorts first is seed-dependent — a fixed
      // assignment reproduces only the seeds it was measured on (the 606/707 battery
      // divergences). Pinning the uid-derived targets reproduces the reference's pre-shuffle
      // seats; start_game's shuffle (seeded by seed^crc32(uids), identical across modes) then
      // maps seats to identical start positions. Retrying while unseated is safe
      // (re-occupying a held slot no-ops; queued occupies apply at +latency).
      g0.createDualPlayerGame([&]() {
        int sec_slot = g0.dualSecondaryUidSortsFirst() ? 0 : 1;
        if (g0.dualSecondarySlot() == -1) g0.dualSecondaryOccupySlot(sec_slot);
        if (g0.g_LocalHumanID() == -1) g0.dualLocalOccupySlot(1 - sec_slot);
        if (g0.g_LocalHumanID() != -1 && !race1_sent) {
          g0.getPlayer(g0.g_LocalHumanID()).setRace(race1);
          race1_sent = true;
        }
        if (g0.dualSecondarySlot() != -1 && !race2_sent) {
          g0.setDualSecondaryRace(race2);
          race2_sent = true;
        }
        if (race1_sent && race2_sent) g0.startGame();
      });

      // Per-side policy (P1_POLICY/P2_POLICY, applied per viewer before each dispatch — no bot change).
      const ViewerPolicies viewer_policies = parse_viewer_policies();

      // The two bot mirrors, each owned entirely by its dispatch thread.
      bot_lane_t lanes[2];
      for (int i = 0; i != 2; ++i) {
        lanes[i].th = std::thread([&gameOwner, &lanes, i]{
          BW::set_thread_viewer(i);
          // LAZY HANDLE CONSTRUCTION (launch-race fix, 2026-08-13). BroodwarImpl_handle's
          // constructor walks every unit slot (GameImpl ctor: getUnit over UNIT_ARRAY_MAX_LENGTH)
          // — running that at thread spawn races the main thread's lobby/startGame() population
          // of the flat units container: an unsynchronised read of half-constructed storage.
          // Measured: 4 launcher SIGSEGVs on 2026-08-13, all with the identical backtrace
          // getUnit → unit_dead → poisoned order_type (0x8080… KERN_INVALID_ADDRESS), one side
          // of a dual game dying at load and forfeiting. Constructing on FIRST DISPATCH is safe
          // by the existing design: go is only ever raised after startGame(), and dispatch is
          // strictly barriered, so the container never mutates while a lane runs.
          std::unique_ptr<BWAPI::BroodwarImpl_handle> h;   // unique_ptr, not optional: this TU is C++14
          auto& lane = lanes[i];
          while (true) {
            bool finishing;
            {
              if (!lane_spin(lane.go)) {
                std::unique_lock<std::mutex> lock(lane.m);
                lane.cv.wait(lock, [&] { return lane.go.load(std::memory_order_acquire); });
              }
              lane.go.store(false, std::memory_order_release);
              if (lane.quit) break;
              finishing = lane.finish;
            }
            if (!h) h.reset(new BWAPI::BroodwarImpl_handle(gameOwner.getGame(i)));
            // Section marker for the runner's log split: dispatch is strictly sequential, so
            // everything this bot prints (its own printf included — which never passes through
            // the Broodwar channel) lands between its marker and the next. Flush both sides so
            // block-buffered stdout cannot smear output across sections.
            printf("SBV%d>\n", i);
            fflush(stdout);
            (*h)->update();
            fflush(stdout);
            lane.module_ok = (*h)->externalModuleConnected;
            if (finishing) {
              // The reference launcher's end sequence, per viewer: the update above ran at
              // the session-over state (firing the in-update MatchEnd with the REAL result —
              // the teardown fallback hardcodes MatchEnd(false)), then onGameEnd + leave.
              (*h)->onGameEnd();
              (*h)->bwgame.leaveGame();
              fflush(stdout);
              {
                std::lock_guard<std::mutex> lock(lane.m);
                lane.done.store(true, std::memory_order_release);
              }
              lane.cv.notify_all();
              return;
            }
            {
              std::lock_guard<std::mutex> lock(lane.m);
              lane.done.store(true, std::memory_order_release);
            }
            lane.cv.notify_all();
          }
          // quit can arrive before the first dispatch — then no handle was ever built and
          // there is no session to end.
          if (h) {
            (*h)->onGameEnd();
            (*h)->bwgame.leaveGame();
          }
          {
            std::lock_guard<std::mutex> lock(lane.m);
            lane.done.store(true, std::memory_order_release);
          }
          lane.cv.notify_all();
        });
      }

      // Belt-and-braces cap (the vs.sh watchdog's role, in-process): bots END themselves at
      // GLITCHCORE_SMOKE_FRAMES; if either viewer's game-over signal fails to propagate, stop a
      // little after the cap instead of spinning forever.
      long frame_cap = 1u << 30;
      if (const char* fc = std::getenv("GLITCHCORE_SMOKE_FRAMES")) {
        long v = std::atol(fc);
        if (v > 0) frame_cap = v + 2000;
      }
      int frames = 0;
      // Per-viewer session-over latches, EDGE-TRIGGERED and sticky: the two-process
      // reference launcher exits its whole process on the FIRST frame its gameOver() reads
      // true — even if the condition is transient (measured: a "defeated" player's surviving
      // worker can complete a pending build order and un-defeat it a few frames later; the
      // reference bot is gone by then, so the dual bot must be too). gameOver() itself is a
      // live computation, hence these latches rather than re-reading it.
      bool lane_over[2] = { false, false };
      // On latch, the lane gets the reference's end sequence: ONE final update at the
      // session-over state (real-result MatchEnd fires there), onGameEnd, leave — then the
      // thread exits and the lane is never dispatched again while the survivor plays on.
      auto finish_lane = [&](int i) {
        lane_over[i] = true;
        printf("dual: viewer %d session over f=%d\n", i, frames);
        fflush(stdout);
        apply_viewer_policy(viewer_policies, i);
        lane_signal_go(lanes[i], false, true);
        lane_wait_done(lanes[i]);
        lanes[i].th.join();
      };
      while ((!lane_over[0] || !lane_over[1]) && frames < frame_cap) {
        if (!lane_over[0] && g0.gameOver()) finish_lane(0);
        if (!lane_over[1] && g1.gameOver()) finish_lane(1);
        if (lane_over[0] && lane_over[1]) break;
        // Strictly sequential per-frame dispatch: bot 0 completes before bot 1 starts, so
        // shared engine state sees one reader/writer at a time and order is deterministic.
        // A latched lane is never dispatched again — its two-process counterpart's process
        // has exited — while the survivor plays on to its own end (victory sweep or cap).
        bool modules_ok = true;
        if (!lane_over[0]) {
          apply_viewer_policy(viewer_policies, 0);
          lane_signal_go(lanes[0]);
          lane_wait_done(lanes[0]);
          modules_ok = modules_ok && lanes[0].module_ok;
        }
        if (!lane_over[1]) {
          apply_viewer_policy(viewer_policies, 1);
          lane_signal_go(lanes[1]);
          lane_wait_done(lanes[1]);
          modules_ok = modules_ok && lanes[1].module_ok;
        }
        if (!modules_ok) {
          printf("dual: a bot module failed to load, exiting\n");
          break;
        }
        g0.nextFrame();
        ++frames;
        if (frames % 10000 == 0) {
          printf("dual: f=%d over0=%d over1=%d\n", frames, (int)lane_over[0], (int)lane_over[1]);
          fflush(stdout);
        }
      }
      printf("dual: loop exit f=%d over0=%d over1=%d\n", frames, (int)g0.gameOver(), (int)g1.gameOver());
      fflush(stdout);

      // Only lanes that never latched (frame-cap or module-failure exits) remain running.
      for (int i = 0; i != 2; ++i) {
        if (lane_over[i]) continue;
        lane_signal_go(lanes[i], true);
        lane_wait_done(lanes[i]);
        lanes[i].th.join();
      }
      printf("dual: game finished after %d frames\n", frames);
      return 0;
    } catch (const std::exception& e) {
      printf("dual error: %s\n", e.what());
      return 1;
    }
}

// SERIAL-K WORKER (docs/design/MULTI_GAME_HOST.md increment 2): plays game specs SEQUENTIALLY
// in this one process — global_st (dat tables, GRP headers; the asset-cache-fed load) and the
// process itself are reused, while every game gets a fresh GameOwner (fresh engine state — no
// reset-completeness surface) and fresh per-game module paths from the spec (fresh dlopen
// images, so bot statics reset). Frame-keyed process globals are invalidated per game via
// mirrorShareNextGame() (the audit's first entry). A game failure logs and the worker CONTINUES
// — a crash costs one game plus wall time; the harness re-queues by seed (deterministic retry).
// Spec line format (both feeds): map|race1|race2|seed|frames|p1_ai|p2_ai|log
//   SB_SERIAL_QUEUE=<file>  fixed queue — every line, in order.
//   SB_SERIAL_SPOOL=<dir>   WORK-STEALING feed (takes precedence): the spool holds one *.spec
//     file per game; workers claim specs by atomic rename() (same-filesystem, so exactly one
//     claimant wins; the renamed file no longer matches *.spec and is never rescanned) and
//     rescan between games until the spool is empty. Fixes the static-partition straggler loss
//     measured on the round-robin queues (a finished worker idled while a 3-game queue ran on).
static bool play_spec_line(const std::string& line, int& played, int& failed) {
  if (line.empty() || line[0] == '#') return true;
  std::array<std::string, 8> f;
  size_t pos = 0;
  for (int i = 0; i != 8; ++i) {
    const size_t bar = line.find('|', pos);
    f[i] = line.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
    if (bar == std::string::npos) { if (i != 7) f[0].clear(); break; }
    pos = bar + 1;
  }
  if (f[0].empty()) { printf("serial: bad spec line: %s\n", line.c_str()); ++failed; return false; }
  ::setenv("BWAPI_CONFIG_AUTO_MENU__MAP", f[0].c_str(), 1);
  ::setenv("BWAPI_CONFIG_AUTO_MENU__RACE", f[1].c_str(), 1);
  ::setenv("GLITCHCORE_P2_RACE", f[2].c_str(), 1);
  ::setenv("OPENBW_GAME_SEED", f[3].c_str(), 1);
  ::setenv("GLITCHCORE_SMOKE_FRAMES", f[4].c_str(), 1);
  ::setenv("BWAPI_CONFIG_AI__AI", f[5].c_str(), 1);
  ::setenv("GLITCHCORE_P2_AI", f[6].c_str(), 1);
  if (!std::freopen(f[7].c_str(), "w", stdout)) { ++failed; return false; }
  BWAPI::mirrorShareNextGame();
  // Per-game wall on stderr (stdout is the game log — digests untouched): the worker-lifetime
  // gradient is the discriminator between scheduler priority decay (games slow monotonically)
  // and structural in-process cost (uniform).
  struct timeval t0, t1;
  ::gettimeofday(&t0, nullptr);
  const int rc = run_one_dual_game();
  ::gettimeofday(&t1, nullptr);
  fflush(stdout);
  fprintf(stderr, "serial: game %d seed=%s wall=%.2f rc=%d\n", played,
          f[3].c_str(), (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6, rc);
  if (rc != 0) ++failed;
  ++played;
  return true;
}

int dual_main() {
  BW::sacrificeThreadForUI([]{
    const char* spool_path = std::getenv("SB_SERIAL_SPOOL");
    const char* queue_path = std::getenv("SB_SERIAL_QUEUE");
    if ((!spool_path || !*spool_path) && (!queue_path || !*queue_path)) {
      return run_one_dual_game();
    }
    int played = 0, failed = 0;
    if (spool_path && *spool_path) {
      const std::string dir = spool_path;
      const std::string claim = ".claimed." + std::to_string((long long)::getpid());
      for (;;) {
        std::vector<std::string> specs;
        DIR* d = ::opendir(dir.c_str());
        if (!d) { fprintf(stderr, "serial: cannot open spool %s\n", dir.c_str()); break; }
        struct dirent* de;
        while ((de = ::readdir(d)) != nullptr) {
          const std::string n = de->d_name;
          if (n.size() > 5 && n.compare(n.size() - 5, 5, ".spec") == 0) specs.push_back(n);
        }
        ::closedir(d);
        if (specs.empty()) break;
        std::sort(specs.begin(), specs.end());
        bool won_one = false;
        for (const std::string& n : specs) {
          const std::string from = dir + "/" + n, to = from + claim;
          if (std::rename(from.c_str(), to.c_str()) != 0) continue;   // lost the race — next
          std::ifstream sf(to);
          std::string line;
          if (std::getline(sf, line)) play_spec_line(line, played, failed);
          won_one = true;
          break;   // rescan after each game (a scan is trivia next to a game)
        }
        (void)won_one;   // lost every race this pass: rescan — a shrunk set or empty ends it
      }
    } else {
      std::ifstream qf(queue_path);
      if (!qf) { printf("serial: cannot open queue %s\n", queue_path); return 1; }
      std::string line;
      while (std::getline(qf, line)) play_spec_line(line, played, failed);
    }
    if (std::freopen("/dev/tty", "w", stdout) == nullptr)
      (void)std::freopen("/dev/null", "w", stdout);
    fprintf(stderr, "serial: queue done, played=%d failed=%d\n", played, failed);
    return failed ? 1 : 0;
  });
  return 0;
}

}

int main() {

  const char* dual = std::getenv("OPENBW_DUAL_HOST");
  if (dual && *dual == '1') return dual_main();

  BW::sacrificeThreadForUI([]{
    try {
      BW::GameOwner gameOwner;

      gameOwner.setPrintTextCallback([](const char* str) {
        std::string s;
        while (*str) {
          char c = *str++;
          if ((unsigned)c >= 0x20 || c == 9 || c == 10 || c == 13) s += c;
        }
        printf("%s\n", s.c_str());
      });

      BWAPI::BroodwarImpl_handle h(gameOwner.getGame());

      do {
        // sb: with OPENBW_CLIENT_SUPPORT=1 the launcher waits pre-game for a BWAPI client to
        // attach over the POSIX shm transport (basil linux-client-support port). Default OFF:
        // the loop is skipped entirely and module games are byte-identical (rule 9).
        const char* sbClientEnv = std::getenv("OPENBW_CLIENT_SUPPORT");
        if (sbClientEnv && sbClientEnv[0] == '1') {
          // Accept the client without pumping processEvents (see Server::waitForClient).
          // No update() here: the first frame must reach the client only once the game is
          // live, or its Game.init reads pre-match state.
          h->server.waitForClient();
        }
        h->autoMenuManager.startGame();

        while (!h->bwgame.gameOver()) {
          h->update();
          h->bwgame.nextFrame();

          if (!h->externalModuleConnected) {
            printf("No module loaded, exiting\n");
            return 1;
          }
        }
        h->update();
        h->onGameEnd();
        // Two-process end grace (defect fix, task #35): a process that exits the moment its
        // own sim reads game-over strands the peer — the first finisher's socket closes
        // while the other sim still needs its remaining frames, the starved sync fires
        // player_dropped mid-victory-sweep, and the DEFEATED side declares itself
        // victorious via opponents==0 (dual battery seed-606 double-win flake; also the
        // task-#29 SBACT tail drift). Pump the sync a fixed frame budget past game-over so
        // BOTH sims complete the identical decided timeline — the dual-host semantics.
        // Bot-invisible: no update()/callbacks here, END lines already printed.
        {
          static const int endGrace = [] {
            const char* v = std::getenv("SB_TWO_PROCESS_END_GRACE");
            return v && *v ? std::atoi(v) : 240;
          }();
          for (int i = 0; i < endGrace && !h->bwgame.gameClosed(); ++i)
            h->bwgame.nextFrame();
        }
        h->bwgame.leaveGame();
      } while (!h->bwgame.gameClosed() && h->autoMenuManager.autoMenuRestartGame != "" && h->autoMenuManager.autoMenuRestartGame != "OFF");
      return 0;
    } catch (const std::exception& e) {
      printf("Error: %s\n", e.what());
      return 1;
    }
  });

  return 0;
}
