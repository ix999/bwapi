
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
  bool go = false, done = false, quit = false;
  bool module_ok = true;
};

void lane_signal_go(bot_lane_t& lane, bool quit = false) {
  {
    std::lock_guard<std::mutex> lock(lane.m);
    lane.go = true;
    lane.quit = quit;
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
      // Seating matches the two-process reference exactly (measured via the start_game state
      // dump): the index-2 uid sorts first in action application and wins slot 0; the local
      // (index-1) client takes slot 1. Pinning those targets reproduces the reference's
      // pre-shuffle state; retrying while unseated is safe (re-occupying a held slot no-ops,
      // and a walker would overshoot — queued occupies apply at +latency and re-seat).
      g0.createDualPlayerGame([&]() {
        if (g0.dualSecondarySlot() == -1) g0.dualSecondaryOccupySlot(0);
        if (g0.g_LocalHumanID() == -1) g0.dualLocalOccupySlot(1);
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
            {
              std::unique_lock<std::mutex> lock(lane.m);
              lane.cv.wait(lock, [&] { return lane.go; });
              lane.go = false;
              if (lane.quit) break;
            }
            h->update();
            lane.module_ok = h->externalModuleConnected;
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
      while ((!g0.gameOver() || !g1.gameOver()) && frames < frame_cap) {
        // Strictly sequential per-frame dispatch: bot 0 completes before bot 1 starts, so
        // shared engine state sees one reader/writer at a time and order is deterministic.
        lane_signal_go(lanes[0]);
        lane_wait_done(lanes[0]);
        lane_signal_go(lanes[1]);
        lane_wait_done(lanes[1]);
        if (!lanes[0].module_ok || !lanes[1].module_ok) {
          printf("dual: a bot module failed to load, exiting\n");
          break;
        }
        g0.nextFrame();
        ++frames;
        if (frames % 10000 == 0) {
          printf("dual: f=%d over0=%d over1=%d\n", frames, (int)g0.gameOver(), (int)g1.gameOver());
          fflush(stdout);
        }
      }
      printf("dual: loop exit f=%d over0=%d over1=%d\n", frames, (int)g0.gameOver(), (int)g1.gameOver());
      fflush(stdout);

      for (auto& lane : lanes) {
        lane_signal_go(lane, true);
        lane_wait_done(lane);
        lane.th.join();
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
