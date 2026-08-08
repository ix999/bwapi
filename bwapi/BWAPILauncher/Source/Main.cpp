
#include "BWAPI/GameImpl.h"
#include "BW/BWData.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>

// ---- Dual-host mode (ENGINE_OPT_DUALHOST.md): OPENBW_DUAL_HOST=1 hosts BOTH bots in this
// process against ONE simulation — no lockstep peer, no IPC, no duplicate sim. Each bot runs
// on its own dispatch thread (thread_local BroodwarImpl + Broodwar bind per thread); the two
// updates are barriered strictly sequentially each frame, then the main thread steps the sim
// once. Env: BWAPI_CONFIG_AI__AI + BWAPI_CONFIG_AUTO_MENU__RACE for bot 0; SBBOT_P2_AI (a
// DISTINCT FILE COPY of the module) + SBBOT_P2_RACE for bot 1.

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
  bool go = false, done = false, quit = false, finish = false;
  bool module_ok = true;
};

void lane_signal_go(bot_lane_t& lane, bool quit = false, bool finish = false) {
  {
    std::lock_guard<std::mutex> lock(lane.m);
    lane.go = true;
    lane.quit = quit;
    lane.finish = finish;
  }
  lane.cv.notify_all();
}

void lane_wait_done(bot_lane_t& lane) {
  std::unique_lock<std::mutex> lock(lane.m);
  lane.cv.wait(lock, [&] { return lane.done; });
  lane.done = false;
}

int dual_main() {
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

      BW::Game g0 = gameOwner.getGame(0);
      BW::Game g1 = gameOwner.getGame(1);

      const char* map = std::getenv("BWAPI_CONFIG_AUTO_MENU__MAP");
      if (!map || !*map) { printf("dual: BWAPI_CONFIG_AUTO_MENU__MAP not set\n"); return 1; }
      const char* p2ai = std::getenv("SBBOT_P2_AI");
      if (!p2ai || !*p2ai) { printf("dual: SBBOT_P2_AI not set (needs a distinct file copy)\n"); return 1; }
      int race1 = race_from_env("BWAPI_CONFIG_AUTO_MENU__RACE", 1);
      int race2 = race_from_env("SBBOT_P2_RACE", 1);

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

      // The two bot mirrors, each owned entirely by its dispatch thread.
      bot_lane_t lanes[2];
      for (int i = 0; i != 2; ++i) {
        lanes[i].th = std::thread([&gameOwner, &lanes, i]{
          BW::set_thread_viewer(i);
          BWAPI::BroodwarImpl_handle h(gameOwner.getGame(i));
          auto& lane = lanes[i];
          while (true) {
            bool finishing;
            {
              std::unique_lock<std::mutex> lock(lane.m);
              lane.cv.wait(lock, [&] { return lane.go; });
              lane.go = false;
              if (lane.quit) break;
              finishing = lane.finish;
            }
            // Section marker for the runner's log split: dispatch is strictly sequential, so
            // everything this bot prints (its own printf included — which never passes through
            // the Broodwar channel) lands between its marker and the next. Flush both sides so
            // block-buffered stdout cannot smear output across sections.
            printf("SBV%d>\n", i);
            fflush(stdout);
            h->update();
            fflush(stdout);
            lane.module_ok = h->externalModuleConnected;
            if (finishing) {
              // The reference launcher's end sequence, per viewer: the update above ran at
              // the session-over state (firing the in-update MatchEnd with the REAL result —
              // the teardown fallback hardcodes MatchEnd(false)), then onGameEnd + leave.
              h->onGameEnd();
              h->bwgame.leaveGame();
              fflush(stdout);
              {
                std::lock_guard<std::mutex> lock(lane.m);
                lane.done = true;
              }
              lane.cv.notify_all();
              return;
            }
            {
              std::lock_guard<std::mutex> lock(lane.m);
              lane.done = true;
            }
            lane.cv.notify_all();
          }
          h->onGameEnd();
          h->bwgame.leaveGame();
          {
            std::lock_guard<std::mutex> lock(lane.m);
            lane.done = true;
          }
          lane.cv.notify_all();
        });
      }

      // Belt-and-braces cap (the vs.sh watchdog's role, in-process): bots END themselves at
      // SBBOT_SMOKE_FRAMES; if either viewer's game-over signal fails to propagate, stop a
      // little after the cap instead of spinning forever.
      long frame_cap = 1u << 30;
      if (const char* fc = std::getenv("SBBOT_SMOKE_FRAMES")) {
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
          lane_signal_go(lanes[0]);
          lane_wait_done(lanes[0]);
          modules_ok = modules_ok && lanes[0].module_ok;
        }
        if (!lane_over[1]) {
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
